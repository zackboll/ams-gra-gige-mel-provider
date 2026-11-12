#pragma once

#include "mono_free_running.hpp"

#include <functional>
#include <string_view>

namespace gige_mel::backend::profiles::mono_free_running {

// All service calls and callbacks are synchronous and borrowed for this call;
// none are retained. The adapter holds its device lock and native lifetime for
// the whole operation and admits lifecycle state before calling. Callbacks
// must not reenter the provider. Backend command/stream ownership stays there.
using ConnectionCheck = std::function<Status()>;
using BeforeWrite = std::function<Status(std::string_view)>;

// Reads the complete profile facts. Only confirmed absence is optional; failed
// queries, unavailable features, access failures and wrong types remain errors.
Result<NativeSnapshot> read_snapshot(device_features::IDeviceFeatures& features);

// Required connection checks surround the snapshot. The post-read check runs
// before inspecting the snapshot result, preserving terminal-loss priority.
Result<CameraConfiguration> read_configuration(device_features::IDeviceFeatures& features,
                                               const NativeSnapshot& envelope,
                                               const ConnectionCheck& check_connection);

// Reads fresh facts, validates the entire request, executes ordered typed
// writes, and returns actual readback. before_write failures are returned
// directly; setter failures preserve their code and add earlier-write/readback
// context. There is no rollback. Both callbacks must be callable; use explicit
// no-op callbacks only when the caller intentionally has no such boundary.
Result<CameraConfiguration> apply_configuration(device_features::IDeviceFeatures& features,
                                                const CameraConfiguration& request,
                                                const NativeSnapshot& envelope,
                                                const ConnectionCheck& check_connection,
                                                const BeforeWrite& before_write);

} // namespace gige_mel::backend::profiles::mono_free_running
