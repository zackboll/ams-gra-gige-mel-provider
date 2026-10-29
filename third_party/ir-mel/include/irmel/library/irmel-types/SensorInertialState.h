//===============================================================================
/// @file  SensorInertialState.h
/// @brief This file includes the SensorInertialState class

#pragma once

#include "IR_Directional.h"
#include "Uncertainty.h"

#include <chrono>

namespace ams::iface::irmel
{
	/// @class SensorInertialState
	/// @brief Information about the inertial state point of the sensor
	/// @Required This class provides data definition in support of required IR MEL functionality to report inertia state
	/// and must be included as-is in all IR MEL implementations
	class SensorInertialState
	{
	public:
		SensorInertialState() = default;
		SensorInertialState(std::chrono::nanoseconds& stime, Quaternion& q, Quaternion& qe, IR_Directional& pos, IR_Directional& vel,
							Uncertainty& unc)
			: systemTime{stime}, q_xyzw{q}, qECEF_xyzw{qe}, sensorPosition{pos}, sensorVelocity{vel}, uncertainties{unc}
		{
		}
		~SensorInertialState() = default;
		SensorInertialState(const SensorInertialState&) = default;
		SensorInertialState(SensorInertialState&&) = default;
		SensorInertialState& operator=(const SensorInertialState&) = default;
		SensorInertialState& operator=(SensorInertialState&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
		}
		[[nodiscard]] const Quaternion& getQ_xyzw() const
		{
			return this->q_xyzw;
		}
		void setQ_xyzw(const Quaternion& q_xyzw_in)
		{
			this->q_xyzw = q_xyzw_in;
		}
		[[nodiscard]] const Quaternion& getQECEF_xyzw() const
		{
			return this->qECEF_xyzw;
		}
		void setQECEF_xyzw(const Quaternion& qECEF_xyzw_in)
		{
			this->qECEF_xyzw = qECEF_xyzw_in;
		}
		[[nodiscard]] const IR_Directional& getSensorPosition() const
		{
			return this->sensorPosition;
		}
		void setSensorPosition(const IR_Directional& sensorPosition_in)
		{
			this->sensorPosition = sensorPosition_in;
		}
		[[nodiscard]] const IR_Directional& getSensorVelocity() const
		{
			return this->sensorVelocity;
		}
		void setSensorVelocity(const IR_Directional& sensorVelocity_in)
		{
			this->sensorVelocity = sensorVelocity_in;
		}
		[[nodiscard]] const Uncertainty& getUncertainties() const
		{
			return this->uncertainties;
		}
		void setUncertainties(Uncertainty uncertainties_in)
		{
			this->uncertainties = uncertainties_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0};
		Quaternion q_xyzw = Quaternion(0, 0, 0, 0);
																 ///< quaternion from sensor to local level (NorthEastDown)
		Quaternion qECEF_xyzw = Quaternion(0, 0, 0, 0);			 ///< quaternion from NorthEastDown to ECEF
		IR_Directional sensorPosition = IR_Directional(0, 0, 0); ///< sensor position in ECEF frame, in meters
		IR_Directional sensorVelocity = IR_Directional(0, 0, 0); ///< sensor velocity in ECEF frame, as meters / second
		Uncertainty uncertainties = Uncertainty(0, 0);			 ///< navigation uncertainties
	};
} // namespace ams::iface::irmel
