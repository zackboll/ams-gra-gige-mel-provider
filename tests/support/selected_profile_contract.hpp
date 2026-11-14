#pragma once

#include <gige_mel/backend.hpp>

#include <algorithm>
#include <stdexcept>

// Identical semantic requests against actual SDK feature fixtures, not an
// assertion that their sensor limits, quantization or transport are identical.
template <class Apply, class Read, class Clear, class Writes>
void selected_profile_contract(Apply apply, Read read, Clear clear, Writes writes) {
    namespace b = gige_mel::backend;
    const auto require = [](bool ok) {
        if (!ok) throw std::runtime_error("selected shared-profile semantic contract failed");
    };
    // Both fixtures can represent 20 Hz exactly, and both begin above it.
    const b::CameraConfiguration valid{300, 240, 0, 0, 20, std::chrono::microseconds{1250}};
    auto invalid = valid;
    invalid.width = 0;
    clear();
    auto rejected = apply(invalid);
    require(!rejected && rejected.error().code == b::ErrorCode::invalid_argument);
    require(writes().empty());
    auto accepted = apply(valid);
    require(accepted && accepted.value() == valid);
    auto actual = read();
    require(actual && actual.value() == valid);
    const auto ordered = writes();
    const auto rate = std::find(ordered.begin(), ordered.end(), "AcquisitionFrameRate");
    const auto exposure = std::find(ordered.begin(), ordered.end(), "ExposureTime");
    require(rate != ordered.end() && exposure != ordered.end() && rate < exposure);
    require(std::count(ordered.begin(), ordered.end(), "AcquisitionFrameRate") == 1);
    require(std::count(ordered.begin(), ordered.end(), "ExposureTime") == 1);
}
