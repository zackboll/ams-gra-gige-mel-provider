#include "pleora_backend.hpp"
#include "pleora_features.hpp"
#include "pleora_stream.hpp"
#include "profiles/mono_free_running_device.hpp"

#include <PvDeviceGEV.h>
#include <PvDeviceEventSink.h>
#include <PvDeviceInfoGEV.h>
#include <PvPipeline.h>
#include <PvStreamGEV.h>
#include <PvSystem.h>

#include <atomic>
#include <map>
#include <mutex>
#include <new>

namespace gige_mel::backend::pleora {
namespace {
namespace profile = profiles::mono_free_running;
std::mutex ownership_mutex;
std::map<std::string, std::weak_ptr<void>, std::less<>> ownership;

struct Link final : PvDeviceEventSink {
    std::atomic<bool> lost{false};
    void OnLinkDisconnected(PvDevice*) override { lost.store(true); }
};

struct State {
    std::mutex mutex;
    Link link;
    std::unique_ptr<PvDeviceGEV> device;
    std::unique_ptr<PvStreamGEV> stream;
    std::unique_ptr<PvPipeline> pipeline;
    std::shared_ptr<void> reservation;
    std::weak_ptr<NativeSource> source;
    profile::NativeSnapshot envelope;
    CameraCapabilities capabilities;
    std::optional<Error> fault;
    bool closed{false}, registered{false}, enabled{false}, command_attempted{false};

    Status connected() const {
        if (closed) return Error{ErrorCode::stopped, "eBUS device logically closed"};
        if (fault) return *fault;
        if (link.lost.load() || !device || !device->IsConnected())
            return Error{ErrorCode::disconnected, "eBUS device connection lost; close and reopen"};
        return {};
    }
    // All callers hold mutex. Every attempted acquisition command is paired
    // with stop, including failed/uncertain starts. Failure retires this session.
    Status stop_locked() {
        std::optional<Error> first;
        const auto record = [&](Status s) { if (!s && !first) first = s.error(); };
        if (device && command_attempted) {
            Features features(*device->GetParameters());
            record(features.command("AcquisitionStop"));
            command_attempted = false;
        }
        if (device && enabled) {
            record(checked(device->StreamDisable(), "StreamDisable"));
            enabled = false;
        }
        if (pipeline) {
            if (pipeline->IsStarted()) record(checked(pipeline->Stop(), "pipeline stop"));
            pipeline.reset();
        }
        if (stream) {
            record(checked(stream->Close(), "stream close"));
            stream.reset();
        }
        if (first) { fault = *first; return *first; }
        return {};
    }
    Status close_locked() {
        if (closed) return fault ? Status(*fault) : Status{};
        auto result = stop_locked();
        if (device) {
            if (registered) {
                auto unregistered = checked(device->UnregisterEventSink(&link), "unregister link sink");
                if (result && !unregistered) result = unregistered;
                registered = false;
            }
            auto disconnected = checked(device->Disconnect(), "Disconnect");
            if (result && !disconnected) result = disconnected;
            device.reset(); // finish heartbeat teardown before releasing reservation
        }
        closed = true;
        if (!result) fault = result.error();
        reservation.reset();
        return result;
    }
    ~State() {
        try { (void)close_locked(); } catch (...) {
            if (device && registered) (void)device->UnregisterEventSink(&link);
            pipeline.reset(); stream.reset(); device.reset();
        }
    }
};

class Source final : public NativeSource {
public:
    explicit Source(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Source() override { try { (void)stop(); } catch (...) {} }
    Result<RunConfiguration> start() override {
        std::lock_guard lock(state_->mutex);
        if (auto s = state_->connected(); !s) return s.error();
        if (state_->pipeline) return Error{ErrorCode::busy, "eBUS acquisition already active"};
        Features features(*state_->device->GetParameters());
        auto snapshot = profile::read_snapshot(features);
        if (!snapshot) return snapshot.error();
        auto frozen = profile::acquisition_preflight(snapshot.value(), state_->envelope);
        if (!frozen) return frozen.error();
        auto mode = features.read_string_or_enum("AcquisitionMode");
        if (!mode) return mode.error();
        if (mode.value() != profile::acquisition_mode)
            return Error{ErrorCode::unsupported, "eBUS requires AcquisitionMode=Continuous; no implicit write"};
        auto payload = features.read_integer("PayloadSize");
        if (!payload) return payload.error();
        auto plan = profile::plan_pool(payload.value(), frozen.value(), 8, 64 * 1024 * 1024,
                                      256 * 1024 * 1024, state_->capabilities.pixel_format);
        if (!plan) return plan.error();
        // Cleanup is required even if stream setup or the start acknowledgement
        // fails. No packet-size negotiation, persistent feature write or retry.
        struct Rollback {
            State& state;
            bool committed{false};
            ~Rollback() { if (!committed) (void)state.stop_locked(); }
        } rollback{*state_};
        state_->stream = std::make_unique<PvStreamGEV>();
        auto r = state_->stream->Open(state_->device->GetIPAddress());
        if (!r.IsOK()) return native_error(r, "stream open");
        r = state_->device->SetStreamDestination(state_->stream->GetLocalIPAddress(), state_->stream->GetLocalPort());
        if (!r.IsOK()) return native_error(r, "SetStreamDestination");
        state_->pipeline = std::make_unique<PvPipeline>(state_->stream.get());
        state_->pipeline->SetHandleBufferTooSmall(false); // never grow beyond admitted budget
        state_->pipeline->SetBufferSize(static_cast<std::uint32_t>(plan.value().buffer_bytes));
        r = state_->pipeline->SetBufferCount(8);
        if (!r.IsOK()) return native_error(r, "SetBufferCount");
        r = state_->pipeline->Start();
        if (!r.IsOK()) return native_error(r, "pipeline start");
        state_->enabled = true; // StreamEnable may have taken effect despite failure
        r = state_->device->StreamEnable();
        if (!r.IsOK()) return native_error(r, "StreamEnable");
        state_->command_attempted = true;
        if (auto s = features.command("AcquisitionStart"); !s) return s.error();
        if (auto s = state_->connected(); !s) return s.error();
        rollback.committed = true;
        return RunConfiguration{frozen.value(), state_->capabilities.pixel_format, plan.value().buffer_bytes};
    }
    Result<NativeBuffer> next(std::uint32_t timeout_ms) override {
        std::unique_lock lock(state_->mutex);
        if (auto s = state_->connected(); !s) return s.error();
        if (!state_->pipeline) return Error{ErrorCode::stopped, "eBUS acquisition stopped"};
        PvBuffer* buffer = nullptr;
        PvResult operation;
        const auto r = state_->pipeline->RetrieveNextBuffer(&buffer, timeout_ms, &operation);
        if (!r.IsOK()) {
            auto error = native_error(r, "RetrieveNextBuffer");
            if (error.code != ErrorCode::timeout) state_->fault = error;
            return error;
        }
        if (!buffer) return Error{ErrorCode::io_error, "eBUS retrieval succeeded without buffer"};
        buffer_guard_ = std::move(lock); // pin graph until the receiving thread releases
        return NativeBuffer{buffer, operation};
    }
    Status release(PvBuffer* buffer) override {
        auto guard = std::move(buffer_guard_);
        auto result = checked(state_->pipeline->ReleaseBuffer(buffer), "ReleaseBuffer");
        if (!result) state_->fault = result.error();
        return result;
    }
    Status stop() override {
        std::lock_guard lock(state_->mutex);
        return state_->stop_locked();
    }
private:
    std::shared_ptr<State> state_;
    std::unique_lock<std::mutex> buffer_guard_;
};

class Device final : public ICameraDevice {
public:
    explicit Device(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Device() override { try { (void)close(); } catch (...) {} }
    CameraCapabilities capabilities() const override { return state_->capabilities; }
    Result<CameraConfiguration> configuration() const override {
        std::lock_guard lock(state_->mutex);
        if (auto s = state_->connected(); !s) return s.error();
        if (state_->pipeline) return Error{ErrorCode::busy, "eBUS configuration snapshot requires stopped acquisition"};
        Features features(*state_->device->GetParameters());
        return profile::read_configuration(features, state_->envelope, [&] { return state_->connected(); });
    }
    Result<CameraConfiguration> apply(const CameraConfiguration& request) override {
        std::lock_guard lock(state_->mutex);
        if (auto s = state_->connected(); !s) return s.error();
        if (state_->pipeline) return Error{ErrorCode::busy, "eBUS configuration writes require stopped acquisition"};
        Features features(*state_->device->GetParameters());
        return profile::apply_configuration(features, request, state_->envelope,
            [&] { return state_->connected(); }, [&](std::string_view) { return state_->connected(); });
    }
    Result<std::unique_ptr<IFrameStream>> create_stream() override {
        std::lock_guard lock(state_->mutex);
        if (auto s = state_->connected(); !s) return s.error();
        if (!state_->source.expired()) return Error{ErrorCode::busy, "eBUS device already has a stream"};
        auto source = std::make_shared<Source>(state_);
        state_->source = source;
        return make_stream(std::move(source));
    }
    Status close() override {
        // Do not destroy a retrieved PvBuffer while Stream copies it. Serialize
        // logical close through the same stream boundary as native stop.
        // Source's buffer ownership guard below drains any in-flight retrieval.
        std::lock_guard lock(state_->mutex);
        return state_->close_locked();
    }
private:
    std::shared_ptr<State> state_;
};

class Backend final : public ICameraBackend {
public:
    std::string_view id() const noexcept override { return "pleora"; }
    Result<std::vector<DeviceInfo>> enumerate() override {
        PvSystem system;
        system.SetDetectionTimeout(1000);
        if (auto s = checked(system.Find(), "discovery"); !s) return s.error();
        std::map<std::string, DeviceInfo> found;
        for (std::uint32_t i = 0; i < system.GetDeviceCount(); ++i) {
            auto* info = dynamic_cast<const PvDeviceInfoGEV*>(system.GetDeviceInfo(i));
            if (!info) continue;
            const std::string mac = info->GetMACAddress().GetAscii();
            if (!mac.empty()) found.emplace(mac, DeviceInfo{"pleora", mac, info->GetDisplayID().GetAscii()});
        }
        std::vector<DeviceInfo> devices;
        for (auto& [mac, info] : found) devices.push_back(std::move(info));
        return devices;
    }
    Result<std::shared_ptr<ICameraDevice>> open(std::string_view id) override {
        bool mac_valid = id.size() == 17;
        for (std::size_t i = 0; mac_valid && i < id.size(); ++i)
            mac_valid = i % 3 == 2 ? id[i] == ':'
                : (id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f');
        if (!mac_valid)
            return Error{ErrorCode::invalid_argument, "eBUS open requires exact discovered MAC address"};
        auto state = std::make_shared<State>();
        {
            std::lock_guard lock(ownership_mutex);
            auto& held = ownership[std::string(id)];
            if (!held.expired()) return Error{ErrorCode::busy, "eBUS camera already owned in this process"};
            state->reservation = std::make_shared<int>(0);
            held = state->reservation;
        }
        PvSystem system;
        system.SetDetectionTimeout(1000);
        if (auto s = checked(system.Find(), "open discovery"); !s) return s.error();
        const PvDeviceInfoGEV* selected = nullptr;
        for (std::uint32_t i = 0; i < system.GetDeviceCount(); ++i) {
            auto* info = dynamic_cast<const PvDeviceInfoGEV*>(system.GetDeviceInfo(i));
            if (info && id == info->GetMACAddress().GetAscii()) { selected = info; break; }
        }
        if (!selected) return Error{ErrorCode::not_found, "eBUS exact MAC not discovered"};
        if (!selected->IsLicenseValid())
            return Error{ErrorCode::unsupported, "eBUS SDK license rejected: " + std::string(selected->GetLicenseMessage().GetAscii())};
        state->device = std::make_unique<PvDeviceGEV>();
        auto r = state->device->Connect(selected, PvAccessExclusive);
        if (!r.IsOK()) return native_error(r, "exclusive Connect");
        r = state->device->RegisterEventSink(&state->link);
        if (!r.IsOK()) return native_error(r, "register link sink");
        state->registered = true;
        Features features(*state->device->GetParameters());
        auto snapshot = profile::read_snapshot(features);
        if (!snapshot) return snapshot.error();
        auto capabilities = profile::capabilities_from(snapshot.value());
        if (!capabilities) return capabilities.error();
        if (auto s = state->connected(); !s) return s.error();
        state->envelope = snapshot.value();
        state->capabilities = capabilities.value();
        return std::shared_ptr<ICameraDevice>(std::make_shared<Device>(std::move(state)));
    }
};
}

std::shared_ptr<ICameraBackend> make_backend() { return std::make_shared<Backend>(); }
} // namespace gige_mel::backend::pleora
