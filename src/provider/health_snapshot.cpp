#include "health_snapshot.hpp"
#include "stream_state.hpp"
#include "software_identity.hpp"
#include <limits>
#include <time.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

// Both Health counters use explicit modulo-2^32 wrap; no signed overflow or
// saturation. Compile-time boundary evidence, not a runtime setter.
constexpr std::uint32_t next_counter(std::uint32_t value) noexcept {
    return static_cast<std::uint32_t>(value + 1U);
}
static_assert(next_counter(0) == 1 && next_counter(std::numeric_limits<std::uint32_t>::max()) == 0);

// Fixed identities in the manager-local experimental profile, not a vendor
// registry. Numeric BIT_Command ID 1 maps explicitly to software_bit_id().
mel::UCI_ID software_bit_id() {
    return {{0x9c,0x85,0xc2,0x63,0x54,0x6e,0x4e,0x11,0x9b,0x71,0xd2,0x40,0x5a,0x13,0x80,0x01},
            "gige_mel.provider_state.v1"};
}
constexpr const char* bit_items[] = {"gige_mel.configuration_known_valid",
    "gige_mel.owned_acquisition_without_terminal_failure"};
constexpr const char* fault_codes[] = {"gige_mel.configuration_invalid", "gige_mel.acquisition_failed"};
constexpr const char* fault_descriptions[] = {"Initialized provider configuration is not known valid",
    "Logically owned Image/Stack acquisition has an observed terminal failure; explicitly disable for cleanup"};

mel::BIT_Configuration bit_catalogue() {
    mel::BIT_Type test;
    test.setBitID(software_bit_id());
    test.setAcceptedInterface(mel::BIT_ControlInterface::SubsystemBITCommand);
    test.setBitItemNames({bit_items[0], bit_items[1]});
    // Empty components and upstream default duration 0 mean unspecified.
    return mel::BIT_Configuration({test});
}

std::chrono::nanoseconds host_utc_now() {
    timespec value{};
    if (::clock_gettime(CLOCK_REALTIME, &value) != 0)
        throw std::runtime_error("BIT host UTC clock_gettime(CLOCK_REALTIME) failed");
    constexpr auto billion = 1000000000LL;
    constexpr auto maximum = std::numeric_limits<std::chrono::nanoseconds::rep>::max();
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= billion ||
        value.tv_sec > maximum / billion ||
        (value.tv_sec == maximum / billion && value.tv_nsec > maximum % billion))
        throw std::runtime_error("BIT host Unix-epoch nanoseconds are not representable");
    const auto count = static_cast<std::chrono::nanoseconds::rep>(value.tv_sec) * billion + value.tv_nsec;
    if (count == 0) throw std::runtime_error("BIT host UTC clock returned an unspecified zero timestamp");
    return std::chrono::nanoseconds(count);
}

// Provider-defined software subsystem profile; see docs/subsystem-status.md.
// Identifier 1 is scoped to this manager's provider software subsystem, not a
// globally allocated physical identifier. Zero criticality/source mean
// "unassigned in this profile", not a safety or mission-criticality rating.
constexpr std::uint32_t provider_subsystem_id = 1;
constexpr std::uint32_t unassigned_criticality = 0;
constexpr std::uint32_t unassigned_version_source = 0;

// Pure mapping from one captured core state; no I/O, no runtime reads. The
// caller assigns statusSeqNum only after this payload has been formed.
ir::SubsystemStatusResp subsystem_report(const HealthSnapshot& s) {
    // Major: this profile currently cannot provide its image acquisition
    // function. It is not a hardware diagnosis; Critical is never substituted.
    const bool unable = !s.configuration_valid || (s.active && s.failed);
    const auto failure = !s.initialized ? ir::Failure::NA : unable ? ir::Failure::Major : ir::Failure::Available;
    const auto mode = !s.initialized || unable ? ir::CSCIMode::Unknown :
        s.tasking ? ir::CSCIMode::Operational : s.active ? ir::CSCIMode::Imaging : ir::CSCIMode::Idle;
    ir::SubsystemCSCIInfo csci;
    csci.setCsci(ProviderSoftware::name);
    csci.setMode(mode);
    // Pinned order: Version(source, majorRevision, minorRevision, engineeringRevision).
    csci.setVersion(ir::Version(unassigned_version_source, ProviderSoftware::major,
                                ProviderSoftware::minor, ProviderSoftware::patch));
    csci.setCriticality(unassigned_criticality);
    csci.setFailureLevel(failure);
    csci.setBIT_report(s.bit_revision);
    // In-process software association only: not camera reachability, SDK
    // transport, heartbeat, packet delivery or a future-acquisition promise.
    csci.setConnectionEstablished(s.initialized);
    ir::SubsystemStatusResp report;
    report.setSubsystemId(provider_subsystem_id);
    report.setCriticality(unassigned_criticality);
    report.setFailureLevel(failure);
    report.setSubsystems({});
    report.setSubsystemCount(0);
    report.setCSCI({csci});
    report.setCsciCount(1);
    return report;
}

// No operations mutex or backend I/O. The temporary acquisition owner is
// destroyed while state_mutex is held, under the established unpublication rule.
// One transaction prepares BIT state (and optionally the subsystem report) from
// the same core capture; candidate state is committed only after formation.
HealthSnapshot health_snapshot(Runtime& r, bool execute, bool prepare_bit,
                               bool prepare_subsystem) {
    std::lock_guard lock(r.state_mutex);
    const auto acquisition = r.acquisition.lock();
    HealthSnapshot s{r.initialized, r.configuration_valid, r.image_active,
        acquisition && acquisition->failed.load(), r.configuration_generation,
        {r.delivered.load(), r.dropped.load(), r.rejected.load(),
         r.callback_failures.load(), r.stream_failures.load(), r.last_error}, {}, {},
        r.bit_revision, {}, {}, r.tasking_active};
    if (!prepare_bit) return s;
    try {
        auto observations = r.fault_observations;
        const bool predicates[] = {s.initialized && !s.configuration_valid,
                                  s.initialized && s.active && s.failed};
        std::vector<mel::Fault> faults;
        for (std::size_t i = 0; i < observations.size(); ++i) {
            if (!predicates[i]) { observations[i].reset(); continue; }
            if (!observations[i]) observations[i] = host_utc_now();
            auto uuid = software_bit_id().getUUID();
            uuid.back() = static_cast<std::uint8_t>(0x11 + i);
            faults.emplace_back(mel::UCI_ID(uuid, fault_codes[i]), mel::FaultSeverity::Warning,
                mel::FaultState::Set, std::vector<mel::FaultData>{}, *observations[i],
                fault_codes[i], fault_descriptions[i], std::vector<mel::UCI_ID>{},
                std::vector<mel::FaultAmbiguityGroup>{});
        }
        auto completion = r.completed_bit;
        if (execute) {
            const bool passes[] = {s.initialized && s.configuration_valid, !(s.active && s.failed)};
            std::vector<mel::CompletedBIT_Item> items;
            for (std::size_t i = 0; i < 2; ++i)
                items.emplace_back(bit_items[i], passes[i] ? mel::BIT_Result::Pass : mel::BIT_Result::Fail,
                    passes[i] ? "" : fault_descriptions[i]);
            const bool pass = passes[0] && passes[1];
            // Completion means item evaluation has finished, not command admission.
            completion = std::make_shared<const mel::CompletedBIT>(software_bit_id(), host_utc_now(),
                pass ? mel::BIT_Result::Pass : mel::BIT_Result::Fail,
                pass ? "" : "Provider observed-state check failed; see item failure reasons", std::move(items));
        }
        s.bit.emplace(std::vector<mel::ActiveBIT>{}, completion ? std::vector<mel::CompletedBIT>{*completion} :
                      std::vector<mel::CompletedBIT>{}, std::move(faults));
        // One revision per committed semantic change: a newly completed
        // execution (even with an identical result/time) and/or an observed
        // fault creation, retirement or recurrence. Unchanged samples keep it.
        const bool changed = execute || observations != r.fault_observations;
        const auto revision = changed ? next_counter(r.bit_revision) : r.bit_revision;
        // Commit only after the complete immutable payload has been formed.
        r.completed_bit = std::move(completion);
        r.fault_observations = observations;
        r.bit_revision = revision;
        s.bit_revision = revision;
    } catch (...) { s.bit_error = std::current_exception(); }
    // A subsystem report requires the paired canonical BIT commit. Its own
    // failure never rolls back that commit and consumes no sequence number.
    if (prepare_subsystem && s.bit) {
        try {
            auto report = subsystem_report(s);
            const auto sequence = next_counter(r.subsystem_sequence);
            report.setStatusSeqNum(sequence);
            s.subsystem.emplace(std::move(report));
            r.subsystem_sequence = sequence;
        } catch (...) { s.subsystem.reset(); s.subsystem_error = std::current_exception(); }
    }
    return s;
}

} // namespace provider_detail
} // namespace gige_mel
