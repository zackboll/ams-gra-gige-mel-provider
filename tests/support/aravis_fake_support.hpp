#pragma once

// TEST-ONLY selection of Aravis's in-process Fake interface (no network, no
// arv-fake-gv-camera process). The SAME adapter implementation is used as for
// GigE; only the interface source differs. Every feature read/write still goes
// through the real Aravis 0.8.36 GenICam implementation of the Fake camera.
// Never installed and not reachable from production factories.

#include "aravis_private.hpp"

#include <memory>
#include <string_view>

namespace aravis_fake {

// Aravis 0.8.36 Fake interface device ID (ARV_FAKE_DEVICE_ID).
inline constexpr std::string_view device_id = "Fake_1";

std::shared_ptr<gige_mel::backend::ICameraBackend> make_backend(gige_mel::backend::aravis::TestHooks hooks = {});

// Path and version of the libaravis actually mapped into this process.
std::string loaded_library_path();

} // namespace aravis_fake
