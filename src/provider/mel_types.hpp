#pragma once

#include <gige_mel/provider.hpp>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

// Private implementation namespace: hidden visibility preserves the old
// anonymous-namespace boundary while allowing separate translation units.
namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
using namespace std::chrono_literals;

} // namespace provider_detail
} // namespace gige_mel
