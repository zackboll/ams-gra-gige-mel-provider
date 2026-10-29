//===============================================================================
/// @file  LineOfSightQuaternion.h
/// @brief This file includes the LineOfSightQuaternion class

#pragma once

#include <chrono>
#include "IR_Directional.h"

namespace ams::iface::irmel
{
	/// @class LineOfSightQuaternion
	/// @brief Indicates a rotation of an axis that represents the line of sight
	/// @Required This class provides data definition in support of required IR MEL functinoality to report rotation of an axis
	/// and must be included as-is in all IR MEL implementations
	class LineOfSightQuaternion
	{
	public:
		LineOfSightQuaternion() = default;
		LineOfSightQuaternion(std::chrono::nanoseconds stime, Quaternion& q, ForwardRightDown& rr) : systemTime{stime}, q_xyzw{q}, pqr_rps{rr}
		{
		}
		~LineOfSightQuaternion() = default;
		LineOfSightQuaternion(const LineOfSightQuaternion&) = default;
		LineOfSightQuaternion(LineOfSightQuaternion&&) = default;
		LineOfSightQuaternion& operator=(const LineOfSightQuaternion&) = default;
		LineOfSightQuaternion& operator=(LineOfSightQuaternion&&) = default;

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
		[[nodiscard]] const ForwardRightDown& getPqr_rps() const
		{
			return this->pqr_rps;
		}
		void setPqr_rps(const ForwardRightDown& pqr_rps_in)
		{
			this->pqr_rps = pqr_rps_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0};
		///< Time of validity of the line of sight data, in nanoseconds
		/// Quaternion representing the line of sight of the aperture at time of system_time_ns.
		/// The quaternion is in [x, y, z, w] order.  Where the axis is given by [x,y,z] / sin(w/2)
		/// and the angle of rotation about the axis is given by acos(w) * 2.  The coordinates are
		/// in the navigation coordinate frame (North/East/Down relative to wander North).
		Quaternion q_xyzw = Quaternion(0, 0, 0, 0);
		/// Rotation rates about the axes of the image coordinate frame.
		/// pqr_rps[0] is rotation about the forward axis.
		/// pqr_rps[1] is rotation about the right axis.
		/// pqr_rps[2] is rotation about the down axis.
		/// All rotation rates are in radians/sec.
		ForwardRightDown pqr_rps = ForwardRightDown(0, 0, 0);
	};
} // namespace ams::iface::irmel
