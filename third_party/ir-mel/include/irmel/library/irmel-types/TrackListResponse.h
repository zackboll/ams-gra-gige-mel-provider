//===============================================================================
/// @file  TrackListResponse.h
/// @brief This file includes the TrackListResponse class definition.

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @enum TrackListResponse
	/// @brief Status indicating PrioritizedTrackListCmd has been received.
	/// @RequiredIfTrack This enumeration class defines data used in conditionally required IR MEL function definitions to report task list response
	/// and must be included as-is in all IR MEL implementations that support Tracks.
	enum class TrackListResponse : std::uint32_t
	{
		None,
		Rejected,
		RejectedExpired,
		Accepted
	};
} // namespace ams::iface::irmel
