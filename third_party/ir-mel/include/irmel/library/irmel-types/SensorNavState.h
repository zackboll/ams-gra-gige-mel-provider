//===============================================================================
/// @file  SensorNavState.h
/// @brief This file includes the SensorNavState class

#pragma once

#include <variant>

#include <array>
#include <optional>
#include <stdexcept>
#include "NavError.h"
#include <irmel/library/irmel-types/IR_Directional.h>
#include <mel/library/NavigationReport.h>
#include <math/geometry/Quaternion.h>

namespace ams::iface::irmel
{
	/// @enum CoordinateSystemType
	/// @brief The coordinate system being used as the frame of reference for this data.
	/// @Optional This enum provides data definition in support of IR MEL functionality to report internal navigation state of the sensor at the time
	/// of frame collection.
	enum class CoordinateSystemType
	{
		LLA,
		ECEF,
		NED_PLATFORM,
		NED_SENSOR
	};

	/// @class SensorNavState
	/// @brief This class can be used by a sensor to report internal navigation state at the time of data collection. It facilitates more precise
	/// interpolation between frames.
	/// @Optional This class provides data definition in support of IR MEL functionality to report internal navigation state of the sensor at the time
	/// of frame collection.
	class SensorNavState
	{
	public:
		SensorNavState() = default;
		SensorNavState(IR_Directional& position_in, NavError& posError_in, IR_Directional& velocity_in, NavError& velError_in, IR_Directional& accel_in,
					   NavError& accelError_in, ams::iface::mel::Euler& orientation_in, NavError& orientationError_in, ams::iface::mel::Euler& orientationVel_in,
					   NavError& orientationVelError_in, ams::iface::mel::Euler& orientationAccel_in, NavError& orientationAccelError_in,
					   CoordinateSystemType& coordinateSystem_in)
			: position{position_in},
			  posError{posError_in},
			  velocity{velocity_in},
			  velError{velError_in},
			  accel{accel_in},
			  accelError{accelError_in},
			  orientation{orientation_in},
			  orientationError{orientationError_in},
			  orientationVel{orientationVel_in},
			  orientationVelError{orientationVelError_in},
			  orientationAccel{orientationAccel_in},
			  orientationAccelError{orientationAccelError_in},
			  coordinateSystem{coordinateSystem_in}
		{
		}
		SensorNavState(IR_Directional& position_in, NavError& posError_in, IR_Directional& velocity_in, NavError& velError_in, IR_Directional& accel_in,
					   NavError& accelError_in, Quaternion& orientation_in, NavError& orientationError_in, Quaternion& orientationVel_in,
					   NavError& orientationVelError_in, Quaternion& orientationAccel_in, NavError& orientationAccelError_in,
					   CoordinateSystemType& coordinateSystem_in)
			: position{position_in},
			  posError{posError_in},
			  velocity{velocity_in},
			  velError{velError_in},
			  accel{accel_in},
			  accelError{accelError_in},
			  orientation{orientation_in},
			  orientationError{orientationError_in},
			  orientationVel{orientationVel_in},
			  orientationVelError{orientationVelError_in},
			  orientationAccel{orientationAccel_in},
			  orientationAccelError{orientationAccelError_in},
			  coordinateSystem{coordinateSystem_in}
		{
		}
		~SensorNavState() = default;
		SensorNavState(const SensorNavState&) = default;
		SensorNavState(SensorNavState&&) = default;
		SensorNavState& operator=(const SensorNavState&) = default;
		SensorNavState& operator=(SensorNavState&&) = default;

		// Position getter/setters
		[[nodiscard]] const IR_Directional& getPosition() const
		{
			return this->position;
		}
		void setPosition(const IR_Directional& position_in)
		{
			this->position = position_in;
		}
		[[nodiscard]] const NavError& getPositionError() const
		{
			return this->posError;
		}
		void setPositionError(const NavError& e)
		{
			this->posError = e;
		}

		// Velocity getters/setters
		[[nodiscard]] const IR_Directional& getVelocity() const
		{
			return this->velocity;
		}
		void setVelocity(const IR_Directional& velocity_in)
		{
			this->velocity = velocity_in;
		}
		[[nodiscard]] const NavError& getVelocityError() const
		{
			return this->velError;
		}
		void setVelocityError(const NavError& e)
		{
			this->velError = e;
		}

		// Acceleration getters/setters
		[[nodiscard]] const IR_Directional& getAccel() const
		{
			return this->accel;
		}
		void setAccel(const IR_Directional& accel_in)
		{
			this->accel = accel_in;
		}
		[[nodiscard]] const NavError& getAccelError() const
		{
			return this->accelError;
		}
		void setAccelError(const NavError& e)
		{
			this->accelError = e;
		}

		// Euler getters/setters
		[[nodiscard]] const ams::iface::mel::Euler& getEulerOrientation() const
		{
			return std::get<ams::iface::mel::Euler>(this->orientation);
		}
		void setEulerOrientation(const ams::iface::mel::Euler& orientation_in)
		{
			this->orientation = orientation_in;
		}
		[[nodiscard]] const ams::iface::mel::Euler& getEulerOrientationVel() const
		{
			return std::get<ams::iface::mel::Euler>(this->orientationVel);
		}
		void setEulerOrientationVel(const ams::iface::mel::Euler& orientationVel_in)
		{
			this->orientationVel = orientationVel_in;
		}
		[[nodiscard]] const ams::iface::mel::Euler& getEulerOrientationAccel() const
		{
			return std::get<ams::iface::mel::Euler>(this->orientationAccel);
		}
		void setEulerOrientationAccel(const ams::iface::mel::Euler& orientationAccel_in)
		{
			this->orientationAccel = orientationAccel_in;
		}

		// Quaternion getters/setters
		[[nodiscard]] const Quaternion& getQuaternionOrientation() const
		{
			return std::get<Quaternion>(this->orientation);
		}
		void setQuaternionOrientation(const Quaternion& orientation_in)
		{
			this->orientation = orientation_in;
		}
		[[nodiscard]] const Quaternion& getQuaternionOrientationVel() const
		{
			return std::get<Quaternion>(this->orientationVel);
		}
		void setQuaternionOrientationVel(const Quaternion& orientationVel_in)
		{
			this->orientationVel = orientationVel_in;
		}
		[[nodiscard]] const Quaternion& getQuaternionOrientationAccel() const
		{
			return std::get<Quaternion>(this->orientationAccel);
		}
		void setQuaternionOrientationAccel(const Quaternion& orientationAccel_in)
		{
			this->orientationAccel = orientationAccel_in;
		}

		/// @brief Returns orientation error object
		[[nodiscard]] const NavError& getOrientationError() const
		{
			return orientationError;
		}
		/// @brief Sets orientation error object
		void setOrientationError(const NavError& e)
		{
			orientationError = e;
		}
		[[nodiscard]] const NavError& getOrientationVelError() const
		{
			return orientationVelError;
		}
		void setOrientationVelError(const NavError& e)
		{
			orientationVelError = e;
		}
		[[nodiscard]] const NavError& getOrientationAccelError() const
		{
			return orientationAccelError;
		}
		void setOrientationAccelError(const NavError& e)
		{
			orientationAccelError = e;
		}

		// Coordinate system
		[[nodiscard]] const CoordinateSystemType& getCoordinateSystem() const
		{
			return coordinateSystem;
		}
		void setCoordinateSystem(CoordinateSystemType cs)
		{
			coordinateSystem = cs;
		}

	private:
		// Position and derivatives
		IR_Directional position = IR_Directional(0, 0, 0);
		NavError posError{0, 0, 0};

		IR_Directional velocity = IR_Directional(0, 0, 0);
		NavError velError{0, 0, 0};

		IR_Directional accel = IR_Directional(0, 0, 0);
		NavError accelError{0, 0, 0};

		// Orientation and derivatives
		std::variant<ams::iface::mel::Euler, Quaternion> orientation = Quaternion(0, 0, 0, 0);
		NavError orientationError{0, 0, 0, 0};

		std::variant<ams::iface::mel::Euler, Quaternion> orientationVel = Quaternion(0, 0, 0, 0);
		NavError orientationVelError{0, 0, 0, 0};

		std::variant<ams::iface::mel::Euler, Quaternion> orientationAccel = Quaternion(0, 0, 0, 0);
		NavError orientationAccelError{0, 0, 0, 0};

		// Current coordinate system of stored data
		CoordinateSystemType coordinateSystem{CoordinateSystemType::LLA};
	};

} // namespace ams::iface::irmel
