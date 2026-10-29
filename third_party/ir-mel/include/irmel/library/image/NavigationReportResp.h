//===============================================================================
/// @file  NavigationReportResp.h
/// @brief This file includes the data defintion the MFA uses to respond with the
/// navigation report.

#pragma once
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <chrono>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
/// Of interest is the Command Type enum class,
/// which documents all commands that can be sent from the MFP to the MFA.
/// The Classes in the file
/// describe how the connections between the MFA and MFP are made.
namespace ams::iface::irmel
{
	/// @class NavigationReportResp
	/// @brief This struct is used to acknowledge that the MFA has received the Navigation Report from the MFP
	/// @Required This class provides data definition in support of required IR MEL functionality to indicate report receival
	/// and must be included as-is in all IR MEL implementations
	class NavigationReportResp
	{
	public:
		NavigationReportResp() = default;
		~NavigationReportResp() = default;
		NavigationReportResp(const NavigationReportResp&) = default;
		NavigationReportResp(NavigationReportResp&&) = default;
		NavigationReportResp& operator=(const NavigationReportResp&) = default;
		NavigationReportResp& operator=(NavigationReportResp&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
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
		std::chrono::nanoseconds systemTime{0};
		std::uint32_t commandID{0};
		std::uint32_t reqId{0};
	};

} // end namespace ams::iface::irmel
  // end module ImageIRMEL
