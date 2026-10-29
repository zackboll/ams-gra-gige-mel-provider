//===============================================================================
/// @file  LOS3D_KinematicsType.h
/// @brief This file includes the LOS3D_KinematicsType class
//===============================================================================
#pragma once

#include <chrono>
#include <utility>
#include <cstdint>
#include "AbsoluteReferencePoint.h"
#include "LOS3D_OrientationCovariance.h"
#include "RelativeSlantRangeLOS3D.h"

namespace ams::iface::irmel
{
	/// @brief Alias for standard pair of doubles to represent the azimuth and elevation angles
	/// @Required Type used to represent azimuth and elevation angles and must be provided as-is
	/// by the implementer for all IR MEL implementations
	using AzElAnglePair = std::pair<double, double>;

	/// @brief Alias for standard pair of doubles to represent the azimuth and elevation uncertainty
	/// @Required Type used to represent azimuth and elevation uncertainty and must be provided as-is
	/// by the implementer for all IR MEL implementations
	using AzElUncertainty = std::pair<double, double>;

	/// @enum ReferenceFrame
	/// @brief Describes the Reference Frame as being from the Body, MFA, or NED
	/// @Required This enumeration provides data definition associated with reporting LOS 3D information
	/// which is required functionality and must be included as-is in all IR MEL implementations
	enum class ReferenceFrame : std::uint32_t
	{
		Body,
		MFA,
		NED
	};

	/// @class LOS_KinematicsType
	/// @brief Line of sight 3D kinematics definitions
	/// @Required This class provides data definitions in support of required IR MEL functionality to report Line of sight 3D kinematics
	/// and must be included as-is in all IR MEL implementations
	class LOS3D_KinematicsType
	{
	public:
		/// @brief Default constructor for LOS3D_KinematicsType
		LOS3D_KinematicsType() = default;
		/// @brief Constructor for LOS3D_Kinematics type that uses the following paramters: relative slant range (LOS3D), orienatation covaraince
		/// matrix, reference frame, the absolute reference point (in LLA), and the timestamp value in nanoseconds
		LOS3D_KinematicsType(const RelativeSlantRangeLOS3D& relativeSlantRangeLOS3D_in, const LOS3D_OrientationCovariance& los3D_OrientationCovariance_in,
							 const ReferenceFrame& referenceFrame_in, const AbsoluteReferencePoint& absoluteReferencePoint_in,
							 std::chrono::nanoseconds timestamp_in)
			: relativeSlantRangeLOS3D{relativeSlantRangeLOS3D_in},
			  los3D_OrientationCovariance{los3D_OrientationCovariance_in},
			  referenceFrame{referenceFrame_in},
			  absoluteReferencePoint{absoluteReferencePoint_in},
			  timestamp{timestamp_in}
		{
		}

		/// @brief Default destructor for LOS3D_KinematicsType
		~LOS3D_KinematicsType() = default;
		/// @brief Default copy constructor for LOS3D_KinematicsType
		LOS3D_KinematicsType(const LOS3D_KinematicsType&) = default;
		/// @brief Default move constructor for LOS3D_KinematicsType
		LOS3D_KinematicsType(LOS3D_KinematicsType&&) = default;
		/// @brief Default copy assignment operator for LOS3D_KinematicsType
		LOS3D_KinematicsType& operator=(const LOS3D_KinematicsType&) = default;
		/// @brief Default move assignment operator for LOS3D_KinematicsType
		LOS3D_KinematicsType& operator=(LOS3D_KinematicsType&&) = default;

		/// @brief Returns the relative slant range LOS 3D
		[[nodiscard]] const RelativeSlantRangeLOS3D& getRelativeSlantRangeLOS3D() const
		{
			return this->relativeSlantRangeLOS3D;
		}
		/// @brief Sets the relative slant range LOS 3D
		void setRelativeSlantRangeLOS3D(const RelativeSlantRangeLOS3D& relativeSlantRangeLOS3D_in)
		{
			this->relativeSlantRangeLOS3D = relativeSlantRangeLOS3D_in;
		}
		/// @brief Returns the LOS 3D Orientation Covariance
		[[nodiscard]] const LOS3D_OrientationCovariance& getLOS3D_OrientationCovariance() const
		{
			return this->los3D_OrientationCovariance;
		}
		/// @brief Sets the LOS 3D Orientation Covariance
		void setLOS3D_OrientationCovariance(const LOS3D_OrientationCovariance& los3D_OrientationCovariance_in)
		{
			this->los3D_OrientationCovariance = los3D_OrientationCovariance_in;
		}
		/// @brief Returns the Reference Frame (Body, MFA, or NED)
		[[nodiscard]] ReferenceFrame getReferenceFrame() const
		{
			return this->referenceFrame;
		}
		/// @brief Sets the Reference Frame (Body, MFA, or NED)
		void setReferenceFrame(const ReferenceFrame& referenceFrame_in)
		{
			this->referenceFrame = referenceFrame_in;
		}
		/// @brief Gets the Azimuth and Elevation Angle Pair
		[[nodiscard]] const AzElAnglePair& getAzElAnglePair() const
		{
			return this->azElAnglePair;
		}
		/// @brief Sets the Azimuth and Elevation Angle Pair
		void setAzElAnglePair(const AzElAnglePair& azElAnglePair_in)
		{
			this->azElAnglePair = azElAnglePair_in;
		}
		/// @brief Gets the Azimuth and Elevation Uncertainty
		[[nodiscard]] const AzElUncertainty& getAzElUncertainty() const
		{
			return this->azElUncertainty;
		}
		/// @brief Sets the Azimuth and Elevation Uncertainty
		void setAzElUncertainty(const AzElUncertainty& azElUncertainty_in)
		{
			this->azElUncertainty = azElUncertainty_in;
		}
		/// @brief Gets the Absolute Reference Point
		[[nodiscard]] AbsoluteReferencePoint getAbsoluteReferencePoint() const
		{
			return this->absoluteReferencePoint;
		}
		/// @brief Sets the Absolute Reference Point
		void setAbsoluteReferencePoint(const AbsoluteReferencePoint& absoluteReferencePoint_in)
		{
			this->absoluteReferencePoint = absoluteReferencePoint_in;
		}
		/// @brief Gets the Timestamp in nanoseconds
		[[nodiscard]] std::chrono::nanoseconds getTimestamp() const
		{
			return this->timestamp;
		}
		/// @brief Sets the Timestamp in nanoseconds
		void setTimestamp(std::chrono::nanoseconds timestamp_in)
		{
			this->timestamp = timestamp_in;
		}

	private:
		// Describes LOS, angular rates and associated errors of the entity from the
		// reference position in terms of a 3D LOS unit vector and a 2D coordinate frame
		// normal to the LOS vector.
		RelativeSlantRangeLOS3D relativeSlantRangeLOS3D;
		// Indicates the 1-sigma covariance of line of sight angles and slant range. The
		// covariance is symmetric and therefore the covariance values expressed here are
		// the minimum set.
		LOS3D_OrientationCovariance los3D_OrientationCovariance;

		// Reference Frame Enumeration that could be Body, MFA, or NED
		ReferenceFrame referenceFrame{ReferenceFrame::Body};

		// Standard pair holding the azimuth and elevation angles
		AzElAnglePair azElAnglePair;

		// Standard pair holding the azimuth and elevation uncertainty
		AzElUncertainty azElUncertainty;

		// Absolute Reference Point defined in LLA
		AbsoluteReferencePoint absoluteReferencePoint;

		// Timestamp value in nanoseconds
		std::chrono::nanoseconds timestamp{0};
	};
} // end namespace  ams::iface::irmel
