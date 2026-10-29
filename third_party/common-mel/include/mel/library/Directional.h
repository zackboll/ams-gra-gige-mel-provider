#pragma once

#include "UCI_ID.h"

namespace ams::iface::mel
{
	/// @class BaseDirectional
	/// @brief TBD
	/// @Required This class provides data definition in support of required MEL functionality to report directional data
	/// and must be included as-is in all MEL implementations.
	class BaseDirectional
	{
	public:
		BaseDirectional() = default;
		BaseDirectional(double x, double y, double z) : xAxis{x}, yAxis{y}, zAxis{z}
		{
		}
		~BaseDirectional() = default;
		BaseDirectional(const BaseDirectional&) = default;
		BaseDirectional(BaseDirectional&&) = default;
		BaseDirectional& operator=(const BaseDirectional&) = default;
		BaseDirectional& operator=(BaseDirectional&&) = default;

		double xAxis{0};
		double yAxis{0};
		double zAxis{0};
	};

	/// @class Directional
	/// @brief TBD
	/// @Required This class provides data definition in support of required MEL functionality to report directional data
	/// and must be included as-is in all MEL implementations.
	class Directional : protected BaseDirectional
	{
	public:
		Directional() = default;
		Directional(double x, double y, double z) : BaseDirectional(x, y, z)
		{
		}
		~Directional() = default;
		Directional(const Directional&) = default;
		Directional(Directional&&) = default;
		Directional& operator=(const Directional&) = default;
		Directional& operator=(Directional&&) = default;

		void SetAllAxis(double x, double y, double z)
		{
			this->xAxis = x;
			this->yAxis = y;
			this->zAxis = z;
		}

		[[nodiscard]] double getxAxis() const
		{
			return this->xAxis;
		}

		void setxAxis(double newValue)
		{
			this->xAxis = newValue;
		}

		[[nodiscard]] double getyAxis() const
		{
			return this->yAxis;
		}

		void setyAxis(double newValue)
		{
			this->yAxis = newValue;
		}

		[[nodiscard]] double getzAxis() const
		{
			return this->zAxis;
		}

		void setzAxis(double newValue)
		{
			this->zAxis = newValue;
		}
	};

	/// @class NorthEastDown
	/// @brief TBD
	/// @Required This class provides data definition in support of required MEL functionality to report directional data
	/// and must be included as-is in all MEL implementations.
	class NorthEastDown : protected BaseDirectional
	{
	public:
		NorthEastDown() = default;
		NorthEastDown(double north, double east, double down) : BaseDirectional(north, east, down)
		{
		}
		~NorthEastDown() = default;
		NorthEastDown(const NorthEastDown&) = default;
		NorthEastDown(NorthEastDown&&) = default;
		NorthEastDown& operator=(const NorthEastDown&) = default;
		NorthEastDown& operator=(NorthEastDown&&) = default;

		void SetAllAxis(double north, double east, double down)
		{
			this->xAxis = north;
			this->yAxis = east;
			this->zAxis = down;
		}

		[[nodiscard]] double getNorth() const
		{
			return this->xAxis;
		}

		void setNorth(double newValue)
		{
			this->xAxis = newValue;
		}

		[[nodiscard]] double getEast() const
		{
			return this->yAxis;
		}

		void setEast(double newValue)
		{
			this->yAxis = newValue;
		}

		[[nodiscard]] double getDown() const
		{
			return this->zAxis;
		}

		void setDown(double newValue)
		{
			this->zAxis = newValue;
		}

		// private:
		//	double north;
		//	double east;
		//	double down;
	};

	/// @class Euler
	/// @brief Euler Angle Sequence describing the orientation or boresight of a Component in the order yaw, pitch, roll.  The angles are
	/// relative to the host System's frame of reference, centered on the System's navigational center. This type is aligned with the OMS/
	/// UCI ComponentOrientationType.
	/// @Required This class provides data definition in support of required MEL functionality to report directional data
	/// and must be included as-is in all MEL implementations.
	class Euler : protected BaseDirectional
	{
	public:
		Euler() = default;
		Euler(double roll, double pitch, double yaw) : BaseDirectional(roll, pitch, yaw)
		{
		}
		~Euler() = default;
		Euler(const Euler&) = default;
		Euler(Euler&&) = default;
		Euler& operator=(const Euler&) = default;
		Euler& operator=(Euler&&) = default;

		void SetAllAxis(double roll, double pitch, double yaw)
		{
			this->xAxis = roll;
			this->yAxis = pitch;
			this->zAxis = yaw;
		}

		[[nodiscard]] double getRoll() const
		{
			return this->xAxis;
		}

		void setRoll(double newValue)
		{
			this->xAxis = newValue;
		}

		[[nodiscard]] double getPitch() const
		{
			return this->yAxis;
		}

		void setPitch(double newValue)
		{
			this->yAxis = newValue;
		}

		[[nodiscard]] double getYaw() const
		{
			return this->zAxis;
		}

		void setYaw(double newValue)
		{
			this->zAxis = newValue;
		}

		// private:
		//	double roll;  ///< in radians
		//	double pitch; ///< in radians
		//	double yaw;	  ///< in radians
	};

	/// @class ComponentLocation
	/// @brief To resolve: offsets should be coming from the MFA to the MFP.
	/// Expect this type of information to come across OMS
	/// @Required This class provides data definition in support of required MEL functionality to report directional data
	/// and must be included as-is in all MEL implementations.
	class ComponentLocation : protected BaseDirectional
	{
	public:
		ComponentLocation() = default;
		ComponentLocation(double offsetX, double offsetY, double offsetZ, ForeignKey& key)
			: BaseDirectional(offsetX, offsetY, offsetZ), locationId{key}
		{
		}

		~ComponentLocation() = default;
		ComponentLocation(const ComponentLocation&) = default;
		ComponentLocation(ComponentLocation&&) = default;
		ComponentLocation& operator=(const ComponentLocation&) = default;
		ComponentLocation& operator=(ComponentLocation&&) = default;

		void SetAllAxis(double offsetX, double offsetY, double offsetZ)
		{
			this->xAxis = offsetX;
			this->yAxis = offsetY;
			this->zAxis = offsetZ;
		}

		[[nodiscard]] double getOffsetX() const
		{
			return this->xAxis;
		}

		void setOffsetX(double newValue)
		{
			this->xAxis = newValue;
		}

		[[nodiscard]] double getOffsetY() const
		{
			return this->yAxis;
		}

		void setOffsetY(double newValue)
		{
			this->yAxis = newValue;
		}

		[[nodiscard]] double getOffsetZ() const
		{
			return this->zAxis;
		}

		void setOffsetZ(double newValue)
		{
			this->zAxis = newValue;
		}

		[[nodiscard]] const ForeignKey& getLocationId() const
		{
			return this->locationId;
		}

		void setLocationId(const ForeignKey& newValue)
		{
			this->locationId = newValue;
		}

	private:
		/// Offset from the navigational center of the System to the Component in the forward direction
		/// of the System (along the nose of the aircraft for example), in meters.
		// double offsetX;
		/// Offset from the navigational center of the System to the Component in the right direction
		/// (perpendicular to the OffsetX axis when viewed from above the System, with positive values
		/// to the right of the OffsetX axis), in meters.
		// double offsetY;
		/// Offset from the navigational center of the System to the Component in the down direction
		/// (perpendicular to the OffsetX-OffsetY plane, with positive values toward the bottom of the System),
		/// in meters.
		//  double offsetZ;
		/// The ID of the pylon, bay, or other logical location where the Component is
		/// mounted to the host System.
		ForeignKey locationId;
	};
} // end namespace ams::iface::mel
