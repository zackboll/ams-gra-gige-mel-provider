//===============================================================================
/// @file  ScanParam.h
/// @brief This file includes the data definition needed to command a scan.

#pragma once

#include <chrono>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <math/geometry/RangeAzEl.h>

#include <irmel/library/irmel-types/ScanType.h>
#include <math/geometry/RangeAzEl.h>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class ScanParam
	/// @brief Parameters needed to command a scan.
	/// See ExecuteTaskData.
	/// @Required This class provides data definition in support of required IR MEL implementations to report scan parameters
	/// and must be included as-is in all IR MEL implementations
	class ScanParam
	{
	public:
		ScanParam() = default;
		ScanParam(bool isElDefinedWithRangeAndAltitude, AzEl c, CoordFrameRef refEl, CoordFrameRef refAz, double sw, double sh, ScanType stype,
				  std::uint32_t sId, double srate, double pRi, double rRi, std::uint32_t maxROI, std::uint32_t minROI,
				  std::uint32_t ElScanCenterAltitude, std::uint32_t ElScanCenterRange, DegradationMethod degT)
			: isElevationDefinedWithRangeAndAltitude{isElDefinedWithRangeAndAltitude},
			  center{c},
			  centFrameRefEl{refEl},
			  centFrameRefAz{refAz},
			  scanWidth{sw},
			  scanHeight{sh},
			  scanType{stype},
			  scanId{sId},
			  scanRate{srate},
			  preferredRevisitInterval{pRi},
			  requiredRevisitInterval{rRi},
			  maxRangeOfInterest{maxROI},
			  minRangeOfInterest{minROI},
			  elevationScanCenterAltitude{ElScanCenterAltitude},
			  elevationScanCenterRange{ElScanCenterRange},
			  degradationType{degT}
		{
		}
		~ScanParam() = default;
		ScanParam(const ScanParam&) = default;
		ScanParam(ScanParam&&) = default;
		ScanParam& operator=(const ScanParam&) = default;
		ScanParam& operator=(ScanParam&&) = default;

		[[nodiscard]] bool getIsDefinedWithRangeAndAltitude() const
		{
			return this->isElevationDefinedWithRangeAndAltitude;
		}
		void setIsDefinedWithRangeAndAltitude(bool isElevationDefinedWithRangeAndAltitude_in)
		{
			this->isElevationDefinedWithRangeAndAltitude = isElevationDefinedWithRangeAndAltitude_in;
		}
		[[nodiscard]] AzEl getCenter() const
		{
			return this->center;
		}
		void setCenter(AzEl center_in)
		{
			this->center = center_in;
		}
		[[nodiscard]] CoordFrameRef getCentFrameRefEl() const
		{
			return this->centFrameRefEl;
		}
		void setCentFrameRefEl(CoordFrameRef centFrameRefEl_in)
		{
			this->centFrameRefEl = centFrameRefEl_in;
		}
		[[nodiscard]] CoordFrameRef getCentFrameRefAz() const
		{
			return this->centFrameRefAz;
		}
		void setCentFrameRefAz(CoordFrameRef centFrameRefAz_in)
		{
			this->centFrameRefAz = centFrameRefAz_in;
		}
		[[nodiscard]] double getScanWidth() const
		{
			return this->scanWidth;
		}
		void setScanWidth(double scanWidth_in)
		{
			this->scanWidth = scanWidth_in;
		}
		[[nodiscard]] double getScanHeight() const
		{
			return this->scanHeight;
		}
		void setScanHeight(double scanHeight_in)
		{
			this->scanHeight = scanHeight_in;
		}
		[[nodiscard]] const ScanType& getScanType() const
		{
			return this->scanType;
		}
		void setScanType(const ScanType& scanType_in)
		{
			this->scanType = scanType_in;
		}
		[[nodiscard]] std::uint32_t getScanId() const
		{
			return this->scanId;
		}
		void setScanId(std::uint32_t scanId_in)
		{
			this->scanId = scanId_in;
		}
		[[nodiscard]] double getScanRate() const
		{
			return this->scanRate;
		}
		void setScanRate(double scanRate_in)
		{
			this->scanRate = scanRate_in;
		}
		[[nodiscard]] double getPreferredRevisitInterval() const
		{
			return this->preferredRevisitInterval;
		}
		void setPreferredRevisitInterval(double preferredRevisitInterval_in)
		{
			this->preferredRevisitInterval = preferredRevisitInterval_in;
		}
		[[nodiscard]] double getRequiredRevisitInterval() const
		{
			return this->requiredRevisitInterval;
		}
		void setRequiredRevisitInterval(double requiredRevisitInterval_in)
		{
			this->requiredRevisitInterval = requiredRevisitInterval_in;
		}
		[[nodiscard]] std::uint32_t getMaxRangeOfInterest() const
		{
			return this->maxRangeOfInterest;
		}
		void setMaxRangeOfInterest(std::uint32_t maxRangeOfInterest_in)
		{
			this->maxRangeOfInterest = maxRangeOfInterest_in;
		}
		[[nodiscard]] std::uint32_t getMinRangeOfInterest() const
		{
			return this->minRangeOfInterest;
		}
		void setMinRangeOfInterest(std::uint32_t minRangeOfInterest_in)
		{
			this->minRangeOfInterest = minRangeOfInterest_in;
		}
		[[nodiscard]] std::uint32_t getElevationScanCenterAltitude() const
		{
			return this->elevationScanCenterAltitude;
		}
		void setElevationScanCenterAltitude(std::uint32_t elevationScanCenterAltitude_in)
		{
			this->elevationScanCenterAltitude = elevationScanCenterAltitude_in;
		}
		[[nodiscard]] std::uint32_t getElevationScanCenterRange() const
		{
			return this->elevationScanCenterRange;
		}
		void setElevationScanCenterRange(std::uint32_t elevationScanCenterRange_in)
		{
			this->elevationScanCenterRange = elevationScanCenterRange_in;
		}
		[[nodiscard]] DegradationMethod getDegredationType() const
		{
			return this->degradationType;
		}
		void setDegredationType(DegradationMethod degradationType_in)
		{
			this->degradationType = degradationType_in;
		}

	private:
		bool isElevationDefinedWithRangeAndAltitude = false; ///< Elevation can be defined with center.Az or with
															 ///< elevationScanCenterAltitude and elevationScanCenterRange.
		AzEl center;										 ///< Pointing command for center of FOR, in radians
		CoordFrameRef centFrameRefEl{
			CoordFrameRef::Inertial}; ///< Specifies whether "elevation center" is expressed as an Inertial or Aircraft frame of reference
		CoordFrameRef centFrameRefAz{
			CoordFrameRef::Aircraft}; ///< Specifies whether "azimuth center" is expressed as an Inertial or Aircraft frame of reference
		double scanWidth{0};		  ///< Scan width, in radians
		double scanHeight{0};		  ///< Scan height, in radians
		ScanType scanType{};
		std::uint32_t scanId{0};
		double scanRate{0.0}; ///< Negative rate is in the direction of decreasing azimuth, positive rate is in the direction of increasing
							  ///< azimuth. Units are radians per second
		double preferredRevisitInterval{0.0}; ///< How often an IR MFA should revisit a target (in seconds)
		double requiredRevisitInterval{0.0};  ///< How often an IR MFA must revisit a target (in seconds)
		/**
		 *	Indicates maximum range of interest (in meters), value may be set by a service, if value set to 0 indicates field is not set.
		 *	un-set fields should be ignored, set fields may be ignored by MEL depending on operational need.
		 */
		std::uint32_t maxRangeOfInterest{0};
		/**
		 * Indicates minimum range of intereset (in meters), value may be set by a service, if value set to 0 indicates field is not set.
		 * un-set fields should be ignored, set fields may be ignored by MEL depending on operational need.
		 */
		std::uint32_t minRangeOfInterest{0};
		/**
		 *	Altitude value used in tandom with elevationScanCenterRange and center.Az to define the elevation for the center point around which the
		 *	volume is stablized to support EElScanStabilization.
		 *	EElScanStabilization value may be set by a service, if value set to 0 indicates field is not set
		 *	un-set fields should be ignored, set fields may be ignored by MEL depending on operational need.
		 */
		std::uint32_t elevationScanCenterAltitude{0};
		/**
		 *	Range value used in tandom with elevationScanCenterAltitude and center.Az to define the elevation for the center point around which the
		 *	volume is stabilized to support EElScanStabilization.
		 *	EElScanStabilization value may be set by a service, if value set to 0 indicates field is not set
		 *	un-set fields should be ignored, set fields may be ignored by MEL depending on operational need.
		 */
		std::uint32_t elevationScanCenterRange{0};

		DegradationMethod degradationType{
			DegradationMethod::CAPACITY_DEGRADATION}; ///< Default method of handling an overload of non-skippable tasking
	};
} // namespace ams::iface::irmel
