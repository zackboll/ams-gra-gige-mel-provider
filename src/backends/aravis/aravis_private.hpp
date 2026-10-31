#pragma once

// PRIVATE seams for the adapter's own SDK tests. Not used by the provider,
// the diagnostic tool or any production path; never installed. Fake-interface
// selection lives in tests/support/aravis_fake_support.cpp, not here.

#include "aravis_backend.hpp"

#include <arv.h>

#include <functional>
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
// not replace any SDK feature read or write. Each runs with the adapter's
// relevant lock held and must not call back into the adapter.
struct TestHooks {
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
};

std::shared_ptr<ICameraBackend> make_backend_for_interface(InterfaceSource source, TestHooks hooks = {});

// Test support only: run fn with the open device's native handle while holding
// the device's operation lock, e.g. to arrange native state or attach a GObject
// weak reference. Returns false (without calling fn) after logical close or
// for a device not created by this adapter. fn must not retain the pointer
// beyond its call except through GObject weak references.
bool with_native_device_for_testing(ICameraDevice& device, const std::function<void(ArvDevice*)>& fn);

} // namespace gige_mel::backend::aravis
