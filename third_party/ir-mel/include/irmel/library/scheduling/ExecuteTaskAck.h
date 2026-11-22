//===============================================================================
/// @file  ExecuteTaskAck.h
/// @brief This file includes the ExecuteTaskAck definition

#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
/// Of interest is the Command Type enum class,
/// which documents all commands that can be sent from the MFP to the MFA.
/// The Classes in the file
/// describe how the connections between the MFA and MFP are made.
namespace ams::iface::irmel
{
	/// @class ExecuteTaskAck
	/// @brief Execution of a task is acknowledged by the implementation
	/// @RequiredIfIRSTTrackTasking This class provides data definition in support of conditionally required IR MEL functionality to report task
	/// acknowledgement and must be included as-is in all IR MEL implementations that support IRST Track Tasking.
	class ExecuteTaskAck
	{
	public:
		ExecuteTaskAck() = default;
		explicit ExecuteTaskAck(std::uint32_t cmdId, std::uint32_t id) : commandID{cmdId}, reqId{id}
		{
		}
		~ExecuteTaskAck() = default;
		ExecuteTaskAck(const ExecuteTaskAck&) = default;
		ExecuteTaskAck(ExecuteTaskAck&&) = default;
		ExecuteTaskAck& operator=(const ExecuteTaskAck&) = default;
		ExecuteTaskAck& operator=(ExecuteTaskAck&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::uint32_t getReqId() const
		{
			return this->reqId;
		}
		void setReqId(std::uint32_t reqId_in)
		{
			this->reqId = reqId_in;
		}

	private:
		std::uint32_t commandID = 0;
		std::uint32_t reqId = 0;
	};

} // end namespace ams::iface::irmel
  // end module SchedulingIRMEL
