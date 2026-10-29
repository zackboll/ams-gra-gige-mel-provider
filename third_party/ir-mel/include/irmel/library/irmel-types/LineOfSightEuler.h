//===============================================================================
/// @file  LineOfSightEuler.h
/// @brief This file includes the LineOfSightEuler class

#pragma once

#include <mel/library/CommonMEL.h>
#include <chrono>

namespace ams::iface::irmel
{
	/// @class LineOfSightEuler
	/// @brief Indicates a euler to discover the line of sight data
	/// @Required This class provides data definition in support of required IR MEL functionality to report line of sight data
	/// and must be included as-is in all IR MEL implementations
	class LineOfSightEuler
	{
	public:
		LineOfSightEuler() = default;
		LineOfSightEuler(std::chrono::nanoseconds stime, ams::iface::mel::Euler& att, ams::iface::mel::Euler& rate)
			: systemTime{stime}, attitude{att}, attitudeRates{rate}
		{
		}
		~LineOfSightEuler() = default;
		LineOfSightEuler(const LineOfSightEuler&) = default;
		LineOfSightEuler(LineOfSightEuler&&) = default;
		LineOfSightEuler& operator=(const LineOfSightEuler&) = default;
		LineOfSightEuler& operator=(LineOfSightEuler&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
		}
		[[nodiscard]] const ams::iface::mel::Euler& getAttitude() const
		{
			return this->attitude;
		}
		void setAttitude(ams::iface::mel::Euler attitude_in)
		{
			this->attitude = attitude_in;
		}
		[[nodiscard]] const ams::iface::mel::Euler& getAttitudeRates() const
		{
			return this->attitudeRates;
		}
		void setAttitudeRates(ams::iface::mel::Euler attitudeRates_in)
		{
			this->attitudeRates = attitudeRates_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0};
		///< Time of validity of the line of sight data, in nanoseconds
		ams::iface::mel::Euler attitude{}; ///< Euler angles representing the line of sight of the aperture at time of systemTime, in radians
		ams::iface::mel::Euler
			attitudeRates{}; ///< Euler angle rates representing the line of sight rate at the aperture at systemTime, in radians per second
	};
} // namespace ams::iface::irmel
