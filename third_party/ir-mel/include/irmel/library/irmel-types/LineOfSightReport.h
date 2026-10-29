//===============================================================================
/// @file  LineOfSightReport.h
/// @brief This file includes the LineOfSightReport class

#pragma once

#include <mel/library/NavigationReport.h>
#include <math/geometry/RangeAzEl.h>
#include <chrono>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class LineOfSightReport
	/// @brief Report sent from the MFA to the MFP with sensor pointing information
	/// and optionally atSpeed, inTolerance, and platformAttitude information as
	/// indicated by the validityFlagBitfield
	/// @Required This class provides data definition in support of required IR MEL functionality to indicate line of sight
	/// and must be included as-is in all IR MEL implementations
	class LineOfSightReport
	{
	public:
		LineOfSightReport() = default;
		~LineOfSightReport() = default;
		LineOfSightReport(const LineOfSightReport&) = default;
		LineOfSightReport(LineOfSightReport&&) = default;
		LineOfSightReport& operator=(const LineOfSightReport&) = default;
		LineOfSightReport& operator=(LineOfSightReport&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
		}
		[[nodiscard]] AzEl getPointingAngle() const
		{
			return this->pointingAngle;
		}
		void setPointingAngle(AzEl pointingAngle_in)
		{
			this->pointingAngle = pointingAngle_in;
		}
		[[nodiscard]] AzEl getPointingAngleRates() const
		{
			return this->pointingAngleRates;
		}
		void setPointingAngleRates(AzEl pointingAngleRates_in)
		{
			this->pointingAngleRates = pointingAngleRates_in;
		}
		[[nodiscard]] bool getAtSpeed() const
		{
			return this->atSpeed;
		}
		void setAtSpeed(bool atSpeed_in)
		{
			this->atSpeed = atSpeed_in;
		}
		[[nodiscard]] bool getInTolerance() const
		{
			return this->inTolerance;
		}
		void setInTolerance(bool inTolerance_in)
		{
			this->inTolerance = inTolerance_in;
		}
		[[nodiscard]] const ams::iface::mel::Euler& getPlatformAttitude() const
		{
			return this->platformAttitude;
		}
		void setPlatformAttitude(ams::iface::mel::Euler platformAttitude_in)
		{
			this->platformAttitude = platformAttitude_in;
		}
		[[nodiscard]] uint32_t getValidityFlagBitfield() const
		{
			return this->validityFlagBitfield;
		}
		void setValidityFlagBitfield(uint32_t validityFlagBitfield_in)
		{
			this->validityFlagBitfield = validityFlagBitfield_in;
		}
		[[nodiscard]] double getImageRotation() const
		{
			return this->imageRotation;
		}
		void setImageRotation(double imageRotation_in)
		{
			this->imageRotation = imageRotation_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0};
		AzEl pointingAngle;						///< relative to the sensor
		AzEl pointingAngleRates;
		bool atSpeed{false};					///< to indicate when the MFA is at scan speed
		bool inTolerance{false};				///< to indicate when the MFA is in tolerance of the desired position
		ams::iface::mel::Euler platformAttitude{};
		uint32_t validityFlagBitfield{0};		///< validity of atSpeed, inTolerance, and platformAttitude
		double imageRotation{0};				///< in radians
	};
} // namespace ams::iface::irmel
