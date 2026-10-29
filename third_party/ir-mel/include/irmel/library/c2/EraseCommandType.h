//===============================================================================
/// @file  EraseCommandType.h
/// @brief This file includes an enum of the types of erase available to an EraseCommand
/// message.

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @enum EraseCommandType
	/// @brief Indicates the type of erase requested in an EraseCommand message.
	/// @Optional This enumeration class provides definition in support of memory erasure
	/// and may be included in IR MEL implementations.
	enum class EraseCommandType : std::uint32_t
	{
		Zeroize,
		Classified_data_erase,
		Disable,
		CDE_AND_Zeroize
	};
} // end namespace ams::iface::irmel
