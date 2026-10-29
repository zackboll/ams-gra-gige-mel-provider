//===============================================================================
/// @file  ChannelType.h
/// @brief This file includes an enum of the types of channels available

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @enum ChannelType
	/// @brief ir::ChannelType
	/// Used in Config to describe the channel.
	/// @note CandidateObjects come out of IRSTTrack or IRSTImage
	/// @Required This enumeration class define data used in IR MEL function defintions to indicate the channel types
	/// and must be included as is in all IR MEL implementations
	enum class ChannelType : std::uint32_t
	{
		IRSTTrack,
		IRSTImage,
		CommandAndControl,
		Scheduling,
		HealthAndStatus,
		Instrumentation,
		StackedImage,
		Reserved1,	  ///< Reserved for later usage
		Reserved2,	  ///< Reserved for later usage
	};
} // end namespace ams::iface::irmel
