#pragma once

#include <chrono>
#include <stdexcept>
#include <cstdint>

#include "Directional.h"

namespace ams::iface::mel
{
	/// @enum PositionSolutionState
	/// @brief Indicates the current position solution being utilized on the aircraft.
	/// @Required This enumeration class defines data used in MEL function definitions to indicate the utilized position solution
	/// and must be included as-is in all MEL implementations.
	enum class PositionSolutionState : std::uint32_t
	{
		NotSet, /// < enum has not been set
		Aligning,
		FreeInertial,
		GPS,
		Blended,
		MaxExclusive ///< maximun enum item
	};

	/// @class AttitudeRate
	/// @brief This type is aligned with the OMS/UCI
	/// PositionReportDetailedMDT.PositionReportData.Kinematics.OrientationRate
	/// Containes Euler AttitudeRate as well as a timestamp
	/// @Required This class provides data definition in support of required MEL functionality to report navigation data
	/// and must be included as-is in all MEL implementations.
	class AttitudeRate
	{
	public:
		AttitudeRate() = default;
		AttitudeRate(const Euler& attr, std::chrono::nanoseconds attrt) : attitudeRate{attr}, attitudeRateTime{attrt}
		{
		}
		~AttitudeRate() = default;
		AttitudeRate(const AttitudeRate&) = default;
		AttitudeRate(AttitudeRate&&) = default;
		AttitudeRate& operator=(const AttitudeRate&) = default;
		AttitudeRate& operator=(AttitudeRate&&) = default;

		[[nodiscard]] const Euler& getAttitudeRate() const
		{
			return this->attitudeRate;
		}

		void setAttitudeRate(const Euler& newValue)
		{
			this->attitudeRate = newValue;
		}

		[[nodiscard]] std::chrono::nanoseconds getAttitudeRateTime() const
		{
			return this->attitudeRateTime;
		}

		void setAttitudeRateTime(std::chrono::nanoseconds newValue)
		{
			this->attitudeRateTime = newValue;
		}

	private:
		Euler attitudeRate{};
		std::chrono::nanoseconds attitudeRateTime{0};
	};

	/// @class PositionVelocityCovariance
	/// @brief This type is for all position and velocity covariance functions
	/// @Required This class provides data definition in support of required MEL functionality to report navigation data
	/// and must be included as-is in all MEL implementations.
	class PositionVelocityCovariance
	{
	public:
		PositionVelocityCovariance() = default;
		PositionVelocityCovariance(double pnpn, double pnpe, double pnpd, double pepe, double pepd, double pdpd, double pnvn, double pnve,
								   double pnvd, double peve, double pevd, double pdvd, double vnvn, double vnve, double vnvd, double veve,
								   double vevd, double vdvd)
			: positionPositionPnPn{pnpn},
			  positionPositionPnPe{pnpe},
			  positionPositionPnPd{pnpd},
			  positionPositionPePe{pepe},
			  positionPositionPePd{pepd},
			  positionPositionPdPd{pdpd},
			  positionVelocityPnVn{pnvn},
			  positionVelocityPnVe{pnve},
			  positionVelocityPnVd{pnvd},
			  positionVelocityPeVe{peve},
			  positionVelocityPeVd{pevd},
			  positionVelocityPdVd{pdvd},
			  velocityVelocityVnVn{vnvn},
			  velocityVelocityVnVe{vnve},
			  velocityVelocityVnVd{vnvd},
			  velocityVelocityVeVe{veve},
			  velocityVelocityVeVd{vevd},
			  velocityVelocityVdVd{vdvd}
		{
		}
		~PositionVelocityCovariance() = default;
		PositionVelocityCovariance(const PositionVelocityCovariance&) = default;
		PositionVelocityCovariance(PositionVelocityCovariance&&) = default;
		PositionVelocityCovariance& operator=(const PositionVelocityCovariance&) = default;
		PositionVelocityCovariance& operator=(PositionVelocityCovariance&&) = default;

		[[nodiscard]] double getPositionPositionPnPn() const
		{
			return this->positionPositionPnPn;
		}

		void setPositionPositionPnPn(double newValue)
		{
			this->positionPositionPnPn = newValue;
		}

		[[nodiscard]] double getPositionPositionPnPe() const
		{
			return this->positionPositionPnPe;
		}

		void setPositionPositionPnPe(double newValue)
		{
			this->positionPositionPnPe = newValue;
		}

		[[nodiscard]] double getPositionPositionPnPd() const
		{
			return this->positionPositionPnPd;
		}

		void setPositionPositionPnPd(double newValue)
		{
			this->positionPositionPnPd = newValue;
		}

		[[nodiscard]] double getPositionPositionPePe() const
		{
			return this->positionPositionPePe;
		}

		void setPositionPositionPePe(double newValue)
		{
			this->positionPositionPePe = newValue;
		}

		[[nodiscard]] double getPositionPositionPePd() const
		{
			return this->positionPositionPePd;
		}

		void setPositionPositionPePd(double newValue)
		{
			this->positionPositionPePd = newValue;
		}

		[[nodiscard]] double getPositionPositionPdPd() const
		{
			return this->positionPositionPdPd;
		}

		void setPositionPositionPdPd(double newValue)
		{
			this->positionPositionPdPd = newValue;
		}

		[[nodiscard]] double getPositionVelocityPnVn() const
		{
			return this->positionVelocityPnVn;
		}

		void setPositionVelocityPnVn(double newValue)
		{
			this->positionVelocityPnVn = newValue;
		}

		[[nodiscard]] double getPositionVelocityPnVe() const
		{
			return this->positionVelocityPnVe;
		}

		void setPositionVelocityPnVe(double newValue)
		{
			this->positionVelocityPnVe = newValue;
		}

		[[nodiscard]] double getPositionVelocityPnVd() const
		{
			return this->positionVelocityPnVd;
		}

		void setPositionVelocityPnVd(double newValue)
		{
			this->positionVelocityPnVd = newValue;
		}

		[[nodiscard]] double getPositionVelocityPeVe() const
		{
			return this->positionVelocityPeVe;
		}

		void setPositionVelocityPeVe(double newValue)
		{
			this->positionVelocityPeVe = newValue;
		}

		[[nodiscard]] double getPositionVelocityPeVd() const
		{
			return this->positionVelocityPeVd;
		}

		void setPositionVelocityPeVd(double newValue)
		{
			this->positionVelocityPeVd = newValue;
		}

		[[nodiscard]] double getPositionVelocityPdVd() const
		{
			return this->positionVelocityPdVd;
		}

		void setPositionVelocityPdVd(double newValue)
		{
			this->positionVelocityPdVd = newValue;
		}

		[[nodiscard]] double getVelocityVelocityVnVn() const
		{
			return this->velocityVelocityVnVn;
		}

		void setVelocityVelocityVnVn(double newValue)
		{
			this->velocityVelocityVnVn = newValue;
		}

		[[nodiscard]] double getVelocityVelocityVnVe() const
		{
			return this->velocityVelocityVnVe;
		}

		void setVelocityVelocityVnVe(double newValue)
		{
			this->velocityVelocityVnVe = newValue;
		}

		[[nodiscard]] double getVelocityVelocityVnVd() const
		{
			return this->velocityVelocityVnVd;
		}

		void setVelocityVelocityVnVd(double newValue)
		{
			this->velocityVelocityVnVd = newValue;
		}

		[[nodiscard]] double getVelocityVelocityVeVe() const
		{
			return this->velocityVelocityVeVe;
		}

		void setVelocityVelocityVeVe(double newValue)
		{
			this->velocityVelocityVeVe = newValue;
		}

		[[nodiscard]] double getVelocityVelocityVeVd() const
		{
			return this->velocityVelocityVeVd;
		}

		void setVelocityVelocityVeVd(double newValue)
		{
			this->velocityVelocityVeVd = newValue;
		}

		[[nodiscard]] double getVelocityVelocityVdVd() const
		{
			return this->velocityVelocityVdVd;
		}

		void setVelocityVelocityVdVd(double newValue)
		{
			this->velocityVelocityVdVd = newValue;
		}

	private:
		double positionPositionPnPn{0};
		double positionPositionPnPe{0};
		double positionPositionPnPd{0};
		double positionPositionPePe{0};
		double positionPositionPePd{0};
		double positionPositionPdPd{0};
		double positionVelocityPnVn{0};
		double positionVelocityPnVe{0};
		double positionVelocityPnVd{0};
		double positionVelocityPeVe{0};
		double positionVelocityPeVd{0};
		double positionVelocityPdVd{0};
		double velocityVelocityVnVn{0};
		double velocityVelocityVnVe{0};
		double velocityVelocityVnVd{0};
		double velocityVelocityVeVe{0};
		double velocityVelocityVeVd{0};
		double velocityVelocityVdVd{0};
	};

	/// @class NavigationReport
	/// @brief This type is aligned with the OMS/UCI PositionReport_Detailed message
	/// NavigationReport is sent from the MFP to the MFA
	/// @Required This class provides data definition in support of required MEL functionality to report navigation data
	/// and must be included as-is in all MEL implementations.
	class NavigationReport
	{
	public:
		NavigationReport() = default;
		NavigationReport(std::chrono::nanoseconds stime, PositionSolutionState st, double lat, double lng, double alt, const Euler& att,
						 const AttitudeRate& attr, const NorthEastDown& spd, const NorthEastDown& acc, double angle, double mgh, double amsl,
						 const PositionVelocityCovariance& pvc)
			: systemTime{stime},
			  state{st},
			  latitude{lat},
			  longitude{lng},
			  altitude{alt},
			  attitude{att},
			  attitudeRate{attr},
			  speed{spd},
			  acceleration{acc},
			  wanderAngle{angle},
			  magneticHeading{mgh},
			  altitudeMSL{amsl},
			  positionVelocityCovarianceUncertainty{pvc}

		{
		}
		~NavigationReport() = default;
		NavigationReport(const NavigationReport&) = default;
		NavigationReport(NavigationReport&&) = default;
		NavigationReport& operator=(const NavigationReport&) = default;
		NavigationReport& operator=(NavigationReport&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}

		void setSystemTime(std::chrono::nanoseconds newValue)
		{
			this->systemTime = newValue;
		}

		[[nodiscard]] const PositionSolutionState& getState() const
		{
			return this->state;
		}

		void setState(PositionSolutionState newValue)
		{
			this->state = newValue;
		}

		[[nodiscard]] double getLatitude() const
		{
			return this->latitude;
		}

		void setLatitude(double newValue)
		{
			this->latitude = newValue;
		}

		[[nodiscard]] double getLongitude() const
		{
			return this->longitude;
		}

		void setLongitude(double newValue)
		{
			this->longitude = newValue;
		}

		[[nodiscard]] double getAltitude() const
		{
			return this->altitude;
		}

		void setAltitude(double newValue)
		{
			this->altitude = newValue;
		}

		[[nodiscard]] const Euler& getAttitude() const
		{
			return this->attitude;
		}

		void setAttitude(const Euler& newValue)
		{
			this->attitude = newValue;
		}

		[[nodiscard]] AttitudeRate getAttitudeRate() const
		{
			return this->attitudeRate;
		}

		void setAttitudeRate(const AttitudeRate& newValue)
		{
			this->attitudeRate = newValue;
		}

		[[nodiscard]] const NorthEastDown& getSpeed() const
		{
			return this->speed;
		}

		void setSpeed(const NorthEastDown& newValue)
		{
			this->speed = newValue;
		}

		[[nodiscard]] const NorthEastDown& getAcceleration() const
		{
			return this->acceleration;
		}

		void setAcceleration(const NorthEastDown& newValue)
		{
			this->acceleration = newValue;
		}

		[[nodiscard]] double getWanderAngle() const
		{
			return this->wanderAngle;
		}

		void setWanderAngle(double newValue)
		{
			this->wanderAngle = newValue;
		}

		[[nodiscard]] double getMagneticHeading() const
		{
			return this->magneticHeading;
		}

		void setMagneticHeading(double newValue)
		{
			this->magneticHeading = newValue;
		}

		[[nodiscard]] double getAltitudeMSL() const
		{
			return this->altitudeMSL;
		}

		void setAltitudeMSL(double newValue)
		{
			this->altitudeMSL = newValue;
		}

		[[nodiscard]] PositionVelocityCovariance getPositionVelocityCovarianceUncertainty() const
		{
			return this->positionVelocityCovarianceUncertainty;
		}

		void setPositionVelocityCovarianceUncertainty(const PositionVelocityCovariance& newValue)
		{
			this->positionVelocityCovarianceUncertainty = newValue;
		}

	private:
		std::chrono::nanoseconds systemTime{0}; ///< System time for this command, in nanoseconds
		PositionSolutionState state{0};
		double latitude{0};						///< in radians
		double longitude{0};					///< in radians
		double altitude{0};						///< in meters
		Euler attitude{};
		AttitudeRate attitudeRate;
		NorthEastDown speed;		///< in meters / sec
		NorthEastDown acceleration; ///< in meter / sec squared
		double wanderAngle{0};		///< in radians
									// Covariance uncertainty values
		double magneticHeading{0};
		double altitudeMSL{0};		///< barometric altitude
		PositionVelocityCovariance positionVelocityCovarianceUncertainty;
	};
} // end namespace ams::iface::mel
