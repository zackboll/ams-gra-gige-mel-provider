//===============================================================================
/// @file  EraseCommand.h
/// @brief This file includes the data definition of the Erase Command.

#pragma once

#include <irmel/library/c2/EraseCommandType.h>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class EraseCommand
	/// @brief Used to command an MFA to perform a memory erase
	/// @Optional This class provides data definition in support of memory erasure
	/// and may be included in IR MEL implementations.
	class EraseCommand
	{
	public:
		EraseCommand() = default;
		~EraseCommand() = default;
		EraseCommand(const EraseCommand&) = default;
		EraseCommand(EraseCommand&&) = default;
		EraseCommand& operator=(const EraseCommand&) = default;
		EraseCommand& operator=(EraseCommand&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}

		[[nodiscard]] const EraseCommandType& getEraseCommandType() const
		{
			return this->eraseCommandType;
		}
		void setEraseCommandType(EraseCommandType eraseCommandType_in)
		{
			this->eraseCommandType = eraseCommandType_in;
		}

	private:
		std::uint32_t commandID{0};
		EraseCommandType eraseCommandType{EraseCommandType::Zeroize};
	};
} // end namespace ams::iface::irmel
