//===============================================================================
/// @file  EventType.h
/// @brief This file includes an enum of the types of events available to the TaskEvent
/// message.

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @enum EventType
	/// @brief Indicates the type of task event
	/// @Optional This enumeration is provided for MFAs that support TaskEvent metadata
	enum class EventType : std::uint32_t
	{
		enumNotSet,	   ///< indicates not set
		SLEW_START,	   ///< indicates a slew start event
		SCAN_START,	   ///< indicates a scan start event
		BREAK_SCAN,	   ///< indicates a break scan event
		Reserved11,	   ///< Reserved for later usage
		Reserved12,	   ///< Reserved for later usage
		ACCEL_START,   ///< indicates an accel start event
		ACCEL_COMPLETE ///< indicates an accel complete event
	};
} // end namespace ams::iface::irmel
