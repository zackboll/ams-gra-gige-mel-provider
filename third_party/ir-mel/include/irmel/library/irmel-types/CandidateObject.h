//===============================================================================
/// @file  CandidateObject.h
/// @brief This file includes the CandidateObject class definition.

#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <chrono>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class CandidateObject
	/// @brief Reports the candidate object to the Subsystem
	/// @RequiredIfTrack This class provides data definition in support of conditionally required IR MEL functionality to report candidate object
	/// and must be included as-is in all IR MEL implementations that support Tracks.
	class CandidateObject
	{
	public:
		CandidateObject() = default;
		~CandidateObject() = default;
		CandidateObject(const CandidateObject&) = default;
		CandidateObject(CandidateObject&&) = default;
		CandidateObject& operator=(const CandidateObject&) = default;
		CandidateObject& operator=(CandidateObject&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
		}
		[[nodiscard]] std::uint32_t getDetectionCategory() const
		{
			return this->detectionCategory;
		}
		void setDetectionCategory(std::uint32_t detectionCategory_in)
		{
			this->detectionCategory = detectionCategory_in;
		}
		[[nodiscard]] std::uint32_t getSensorIndex() const
		{
			return this->sensorIndex;
		}
		void setSensorIndex(std::uint32_t sensorIndex_in)
		{
			this->sensorIndex = sensorIndex_in;
		}
		[[nodiscard]] const RowCol& getSubpixel() const
		{
			return this->subpixel;
		}
		void setSubpixel(const RowCol& subpixel_in)
		{
			this->subpixel = subpixel_in;
		}
		[[nodiscard]] double getIntensity() const
		{
			return this->intensity;
		}
		void setIntensity(double intensity_in)
		{
			this->intensity = intensity_in;
		}
		[[nodiscard]] const IR_Directional& getSenRelUnit() const
		{
			return this->senRelUnit;
		}
		void setSenRelUnit(const IR_Directional& senRelUnit_in)
		{
			this->senRelUnit = senRelUnit_in;
		}
		[[nodiscard]] double getSignalToInterferenceRatio() const
		{
			return this->signalToInterferenceRatio;
		}
		void setSignalToInterferenceRatio(double signalToInterferenceRatio_in)
		{
			this->signalToInterferenceRatio = signalToInterferenceRatio_in;
		}
		[[nodiscard]] double getSignalToNoiseRatio() const
		{
			return this->signalToNoiseRatio;
		}
		void setSignalToNoiseRatio(double signalToNoiseRatio_in)
		{
			this->signalToNoiseRatio = signalToNoiseRatio_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0}; ///< Time of validity, nanoseconds
		std::uint32_t detectionCategory{0};		///< Bitfield to identify which waveband temporal depth the detection is located
		std::uint32_t sensorIndex{0};			///< index indicating sensor position
		/// Sub-pixel column, row position of the candidate object.
		/// x = column
		/// y = row
		RowCol subpixel;
		double intensity{0}; ///< Merged intensity of the candidate object
		/// Unit vector components of detection position in sensor-relative
		/// frame LOS, stored as (x,y,z)
		IR_Directional senRelUnit;
		double signalToInterferenceRatio{0}; ///< SIR
		double signalToNoiseRatio{0};		 ///< SNR
	};
} // namespace ams::iface::irmel
