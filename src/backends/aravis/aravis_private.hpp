#pragma once

// PRIVATE seams for the adapter's own SDK tests. Not used by the provider,
// the diagnostic tool or any production path; never installed. Fake-interface
// selection lives in tests/support/aravis_fake_support.cpp, not here.

#include "aravis_backend.hpp"
#include "camera_profile.hpp"

#include <arv.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gige_mel::backend::aravis {

// Where discovery/open go. instance() returns Aravis's borrowed interface
// singleton; it is called lazily (never during factory construction) and the
// backend keeps its own reference afterwards.
struct InterfaceSource {
    std::string name;
    ArvInterface* (*instance)();
};

// Narrow observation/arrangement points at the native-call boundary. They do
// not replace any SDK feature read or write. Unless stated otherwise, each runs
// with the adapter's relevant lock held and must not call back into the adapter.
struct TestHooks {
    // Exact-ID open only: called when a real try_lock of the per-interface lock
    // failed because another thread holds it, immediately before this thread
    // blocks on that lock. Runs with NO adapter lock held. Observation only.
    std::function<void(std::string_view device_id)> interface_lock_contended;
    // After the ownership reservation, before the native open. May throw to
    // exercise exception-safe reservation release.
    std::function<void(std::string_view device_id)> after_reservation;
    // After the native open, before profile validation (e.g. arrange native
    // state through the real SDK, or attach a GObject weak reference).
    std::function<void(ArvDevice*)> after_native_open;
    // Immediately before each native feature write in apply(). Used to change a
    // dynamic native constraint mid-transaction through the real SDK; the SDK
    // itself then accepts or rejects the write.
    std::function<void(std::string_view feature, ArvDevice*)> before_write;

    // ---- Acquisition (GIGE-MEL-009). Observation/arrangement only.
    // Immediately before executing AcquisitionStart or AcquisitionStop, after
    // the adapter's own command validation, with the device lock held. May
    // change native state through the real SDK (e.g. an imposed access mode)
    // so that the SDK itself then rejects the command.
    std::function<void(std::string_view command, ArvDevice*)> before_command;
    // GIGE-MEL-009-R1. TEST-ONLY COMMAND-RESULT BOUNDARY: called only after
    // the SDK executed AcquisitionStart/AcquisitionStop SUCCESSFULLY (its
    // camera effect genuinely happened), device lock held. A returned Error
    // replaces that success, modelling an uncertain native command result
    // after a genuine effect; the ORDINARY production failure path then makes
    // every recovery decision. It is not packet-loss or lost-acknowledgment
    // qualification and implements no recovery itself.
    std::function<std::optional<Error>(std::string_view command, ArvDevice*)> command_result;
    // After the SDK constructed a run's native stream (its receiver thread is
    // already running), device lock held. E.g. attach a GObject weak reference.
    std::function<void(ArvStream*)> after_stream_created;
    // After each pool buffer is allocated, before it is queued; device lock held.
    std::function<void(ArvBuffer*)> after_buffer_allocated;
    // On the receiving thread with NO adapter lock held, immediately before
    // each native timed pop (one polling slice). Observation only.
    std::function<void()> before_native_wait;
    // DECODED-FACTS SEAM, on the receiving thread with no adapter lock held:
    // may edit the facts decoded from a REAL popped SDK buffer before the
    // production validator sees them (incomplete status, chunks, multipart,
    // padding, short sizes the pinned Fake cannot produce). It never touches
    // the SDK buffer, so pixels and ownership stay genuine. Tests that use it
    // must say so.
    std::function<void(aravis_profile::NativeFrameFacts&)> decoded_facts;
};

std::shared_ptr<ICameraBackend> make_backend_for_interface(InterfaceSource source, TestHooks hooks = {});

// Test support only: run fn with the open device's native handle while holding
// the device's operation lock, e.g. to arrange native state or attach a GObject
// weak reference. Returns false (without calling fn) after logical close or
// for a device not created by this adapter. fn must not retain the pointer
// beyond its call except through GObject weak references.
bool with_native_device_for_testing(ICameraDevice& device, const std::function<void(ArvDevice*)>& fn);

} // namespace gige_mel::backend::aravis
