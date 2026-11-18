#pragma once

// GIGE-MEL-GENTL-002: Aravis 0.8.36 GenApi (ArvGc) over a GenTL device port.
// Internal and uninstalled; built only with GIGE_MEL_BUILD_GENTL_BACKEND=ON.
// No Aravis, GLib or GenTL type crosses this header.
//
// Ownership (outermost first):
//   GenApiSession -> GenTL-backed ArvDevice (one reference) -> ArvGc (sole owner)
//   GenApiSession -> gentl::Device -> Interface -> System -> Producer
// The ArvGc is created by, and owned only by, its ArvDevice and is released in
// the device's finalizer BEFORE the parent finalizer runs, so the ArvGc never
// observes a destroyed device. The session releases its ArvDevice before its
// gentl::Device reference, so the producer outlives every GenApi object.
//
// Concurrency: every GenApi operation runs inside transaction(), which holds
// the session mutex for the ENTIRE callback (a complete profile operation).
// Each port access made by Aravis from the ArvDevice virtual methods then takes
// the consumer device mutex as a leaf lock for that one GCReadPort/GCWritePort
// only: lock order is always session -> device, never the reverse, and no
// callback runs under the device mutex. A virtual-method call outside the
// owning transaction thread, or a nested transaction, is refused with an
// error (no native call) instead of deadlocking.

#include "device_features.hpp"
#include "gentl/gentl_consumer.hpp"

#include <gige_mel/backend.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gige_mel::backend::gentl_backend {

// Bounds applied before any XML allocation. The Cognex CIC-5000R XML is
// 1,108,591 bytes; 16 MiB leaves margin while bounding memory and fits the
// Aravis int-sized parser argument.
struct XmlLimits {
    std::uint64_t max_bytes{16u << 20};
};

// Facts about the selected device description (diagnostics only).
struct XmlFacts {
    std::string url;
    std::uint32_t location_index{0};
    std::uint64_t address{0};
    std::uint64_t length{0};        // as declared by the URL
    std::uint64_t parsed_length{0}; // after removing trailing NUL padding only
    bool sha1_verified{false};      // only when the producer supplies a SHA-1
    std::string skipped;            // explicitly unsupported locations, with reasons
    std::string schema;             // RegisterDescription Schema{Major,Minor,SubMinor}Version
    // Schema < 1.1: no GigE legacy byte-order heuristic is applied (see design).
    bool legacy_schema{false};
};

class GenApiSession;
Status unbound_read_for_testing(GenApiSession&); // defined only in test builds

class GenApiSession {
public:
    using Transaction = std::function<Status(device_features::IDeviceFeatures&)>;

    // Explicit open-time GenApi construction from an opened GenTL device:
    // declared port byte order, URL selection, bounded XML retrieval and
    // validation, then ArvGc construction. No fabricated features.
    // `access` must be the access the device was ACTUALLY opened with; a
    // read_only session never issues GCWritePort (writes fail unsupported).
    static Result<std::unique_ptr<GenApiSession>> open(std::shared_ptr<gentl::Device> device, gentl::Access access,
                                                       XmlLimits limits = {});

    ~GenApiSession();
    GenApiSession(const GenApiSession&) = delete;
    GenApiSession& operator=(const GenApiSession&) = delete;

    // Synchronous and serialized. The IDeviceFeatures reference is borrowed
    // for the callback only. Nested calls (same thread) are invalid_state.
    Status transaction(const Transaction& callback);

    // Releases the GenApi objects (ArvGc then ArvDevice). Idempotent and
    // never performs native I/O. Later transactions return ErrorCode::stopped.
    // Returns the consumer device so the caller can close the native chain.
    std::shared_ptr<gentl::Device> release() noexcept;

    [[nodiscard]] const XmlFacts& xml() const noexcept { return xml_; }

private:
    struct Native;
    friend Status unbound_read_for_testing(GenApiSession&);
    GenApiSession() = default;
    std::mutex mutex_;
    std::shared_ptr<gentl::Device> device_;
    std::unique_ptr<Native> native_; // declared after device_: destroyed first
    XmlFacts xml_;
    bool control_{false};
    // Thread currently inside transaction(); read before locking to refuse
    // same-thread reentry instead of self-deadlocking.
    std::atomic<std::thread::id> owner_{};
};

// Live GenApi object accounting for lifetime tests and diagnostics.
struct GenApiObjectCounts {
    long devices{0};
    long genicams{0};
    long order_violations{0}; // an ArvGc finalized after its ArvDevice began finalizing
};
GenApiObjectCounts genapi_object_counts() noexcept;

#ifdef GIGE_MEL_GENTL_TESTING
// Test-only: invoke the ArvDevice read_memory virtual method directly,
// outside any transaction, to prove the unbound path is refused.
Status unbound_read_for_testing(GenApiSession&);
// Test-only: XML text decisions without a producer.
Result<std::string> classify_xml_for_testing(std::string bytes);
#endif

} // namespace gige_mel::backend::gentl_backend
