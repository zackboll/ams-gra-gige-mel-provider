//===============================================================================
/// @file  CalibrationStatusCmd.h
/// @brief This file includes the data definition of the Calibration Status Command.

#pragma once

#include <cstdint>

namespace ams::iface::irmel
{
	/// @class CalibrationStatusCmd
	/// @brief  Used to request Calibration Status for the MFA.
	/// @Optional This class provides data definition in support of IR MEL functionality to request the
	/// Calibration Status of the MFA.
	class CalibrationStatusCmd
	{
	public:
		CalibrationStatusCmd() = default;
		~CalibrationStatusCmd() = default;
		CalibrationStatusCmd(const CalibrationStatusCmd&) = default;
		CalibrationStatusCmd(CalibrationStatusCmd&&) = default;
		CalibrationStatusCmd& operator=(const CalibrationStatusCmd&) = default;
		CalibrationStatusCmd& operator=(CalibrationStatusCmd&&) = default;

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
