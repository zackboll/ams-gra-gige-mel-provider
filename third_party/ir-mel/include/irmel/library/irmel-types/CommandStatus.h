//===============================================================================
/// @file  CommandStatus.h
/// @brief This file includes the status of a command as reported by the MFA. Aligns with OMS CommandStatus.

#pragma once

#include <cstdint>

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <string>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{

	/// @class CommandStatus
	/// @brief Identifies a command message that a Subsystem has identified
	/// @Required This class provides data definition in support of IR MEL functionality to report command status
	/// and must be included as-is in all IR MEL implementations
	class CommandStatus
	{
	public:
		CommandStatus() = default;
		CommandStatus(std::uint32_t cmdId, CommandState& st, CannotComply& reas, std::string& desc)
			: commandID(cmdId), state{st}, reasonID{reas}, reasonDescription{desc}
		{
		}
		~CommandStatus() = default;
		CommandStatus(const CommandStatus&) = default;
		CommandStatus(CommandStatus&&) = default;
		CommandStatus& operator=(const CommandStatus&) = default;
		CommandStatus& operator=(CommandStatus&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] const CommandState& getState() const
		{
			return this->state;
		}
		void setState(CommandState state_in)
		{
			this->state = state_in;
		}
		[[nodiscard]] const CannotComply& getReasonID() const
		{
			return this->reasonID;
		}
		void setReasonID(CannotComply reasonID_in)
		{
			this->reasonID = reasonID_in;
		}
		[[nodiscard]] const std::string& getReasonDescription() const
		{
			return this->reasonDescription;
		}
		void setReasonDescription(const std::string& reasonDescription_in)
		{
			this->reasonDescription = reasonDescription_in;
		}

	private:
		std::uint32_t commandID{0}; ///< Indicates the unique ID of the Subsystem command corresponding to this status message.
		/// Indicates the state of the Subsystem command.  The command state machine assumes that
		/// the Command and CommandStatus messaging interaction is only used to communicate the
		/// desired Command to the Subsystem.  The state machine and messaging terminates as soon
		/// as the host accepts or rejects the Command.  Subsequent reporting of the current/latest
		/// BIT status is given in the SubsystemBIT_Status message.  See enumeration annotations for further details.
		CommandState state{CommandState::NotSet};
		/// Indicates the reason why an action (Task, [Capability]Command, [SupportingCapability]Command,
		/// [Capability]Activity, etc.) was rejected, interrupted, unallocated, failed and/or generally
		/// can't be completed satisfactorily.  See enumeration annotations for further details. This element
		/// is only expected when the Command is in a potentially undesirable state such as rejected, removed
		/// or accepted with less than optimum results expected. A value of zero indicates that  this optional
		/// field is not set.
		CannotComply reasonID{CannotComply::NotSet};
		std::string reasonDescription; ///< human-readable description of the reason for the "can't comply".
	};
} // namespace ams::iface::irmel
