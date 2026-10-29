//===============================================================================
/// @file  CalibrationConfigurationCmd.h
/// @brief This file includes the data definition of the Calibration Configuration Command.

#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <chrono>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class CalibrationConfigurationCmd
	/// @brief  Used to request Calibration Configuration for the MFA.
	/// @Optional This class provides data definition in support of IR MEL functionality to request the
	/// Calibration Configuration of the MFA.
	class CalibrationConfigurationCmd
	{
	public:
		CalibrationConfigurationCmd() = default;
		~CalibrationConfigurationCmd() = default;
		CalibrationConfigurationCmd(const CalibrationConfigurationCmd&) = default;
		CalibrationConfigurationCmd(CalibrationConfigurationCmd&&) = default;
		CalibrationConfigurationCmd& operator=(const CalibrationConfigurationCmd&) = default;
		CalibrationConfigurationCmd& operator=(CalibrationConfigurationCmd&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::uint32_t getRequestID() const
		{
			return this->requestID;
		}
		void setRequestID(std::uint32_t requestID_in)
		{
			this->requestID = requestID_in;
		}

	private:
		std::uint32_t commandID{0};
		std::uint32_t requestID{0};
	};
} // end namespace ams::iface::irmel
