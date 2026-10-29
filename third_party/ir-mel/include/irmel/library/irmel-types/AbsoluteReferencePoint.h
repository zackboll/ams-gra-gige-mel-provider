//===============================================================================
/// @file  AbsoluteReferencePoint.h
/// @brief This file includes the AbsoluteReferencePoint class
//===============================================================================
#pragma once

namespace ams::iface::irmel
{
	/// @class AbsoluteReferencePoint
	/// @brief Location of platform relative to the calculated angles at the time of the tracking estimate referenced in latitude, longitude and
	/// altitude
	/// @Required This class provides data definition in support of required IR MEL functionality to report estimated reference point
	/// and must be included as-is in all IR MEL implementations
	class AbsoluteReferencePoint
	{
	public:
		/// @brief Default constructor for Absolute Reference Point
		AbsoluteReferencePoint() = default;

		/// @brief Creates an Absolute Reference Point taking latitude longitude and altitude as parameters
		AbsoluteReferencePoint(double latitude_in, double longitude_in, double altitude_in) : latitude{latitude_in}, longitude{longitude_in}, altitude{altitude_in}
		{
		}

		/// @brief Default destructor for Absolute Reference Point
		~AbsoluteReferencePoint() = default;

		/// @brief Default copy constructor for Absolute Reference Point
		AbsoluteReferencePoint(const AbsoluteReferencePoint&) = default;

		/// @brief Default move constructor for Absolute Reference Point
		AbsoluteReferencePoint(AbsoluteReferencePoint&&) = default;

		/// @brief Default copy assignment operator for Absolute Reference Point
		AbsoluteReferencePoint& operator=(const AbsoluteReferencePoint&) = default;

		/// @brief Default move assignment operator for Absolute Reference Point
		AbsoluteReferencePoint& operator=(AbsoluteReferencePoint&&) = default;

		/// @brief Returns the latitude
		[[nodiscard]] double getLatitude() const
		{
			return this->latitude;
		}
		/// @brief Sets the latitude
		void setLatitude(double latitude_in)
		{
			this->latitude = latitude_in;
		}
		/// @brief Returns the longitude
		[[nodiscard]] double getLongitude() const
		{
			return this->longitude;
		}
		/// @brief Sets the longitude
		void setLongitude(double longitude_in)
		{
			this->longitude = longitude_in;
		}
		/// @brief Returns the altitude in radians
		[[nodiscard]] double getAltitude() const
		{
			return this->altitude;
		}
		/// @brief Sets the altitude in radians
		void setAltitude(double altitude_in)
		{
			this->altitude = altitude_in;
		}

	private:
		double latitude{0};	 // Latitude value in radians
		double longitude{0}; // Longitude value in radians
		double altitude{0};	 // Altitude value in radians
	};
} // namespace ams::iface::irmel
