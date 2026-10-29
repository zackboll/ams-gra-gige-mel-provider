//===============================================================================
/// @file  CandidateObjectPreProc.h
/// @brief This file includes the data definition of the pre-processed candidate object.

#pragma once

#include <irmel/library/irmel-types/SensorInertialState.h>

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <array>
#include <cstdint>
#include <chrono>

namespace ams::iface::irmel
{
	/// @class CandidateObjectPreProc
	/// @brief Reports the beginning process for and image stream
	/// @RequiredIfBuiltInTracker This class provides data definition in support of required IR MEL functionality to report candidate object
	/// and must be included as-is in all IR MEL implementations
	class CandidateObjectPreProc
	{
	public:
		CandidateObjectPreProc() = default;
		~CandidateObjectPreProc() = default;
		CandidateObjectPreProc(const CandidateObjectPreProc&) = default;
		CandidateObjectPreProc(CandidateObjectPreProc&&) = default;
		CandidateObjectPreProc& operator=(const CandidateObjectPreProc&) = default;
		CandidateObjectPreProc& operator=(CandidateObjectPreProc&&) = default;
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
		[[nodiscard]] const std::array<std::array<std::int16_t, 3>, 3>& getCandidateObjectWithBackground() const
		{
			return this->candidateObjectWithBackground;
		}
		void setCandidateObjectWithBackground(const std::array<std::array<std::int16_t, 3>, 3>& candidateObjectWithBackground_in)
		{
			this->candidateObjectWithBackground = candidateObjectWithBackground_in;
		}
		[[nodiscard]] double getClutter() const
		{
			return this->clutter;
		}
		void setClutter(double clutter_in)
		{
			this->clutter = clutter_in;
		}
		[[nodiscard]] double getCandidateObjectQuality() const
		{
			return this->candidateObjectQuality;
		}
		void setCandidateObjectQuality(double candidateObjectQuality_in)
		{
			this->candidateObjectQuality = candidateObjectQuality_in;
		}
		[[nodiscard]] double getSirDelta() const
		{
			return this->sirDelta;
		}
		void setSirDelta(double sirDelta_in)
		{
			this->sirDelta = sirDelta_in;
		}
		[[nodiscard]] const SensorInertialState& getInertialState() const
		{
			return this->inertialState;
		}
		void setInertialState(const SensorInertialState& inertialState_in)
		{
			this->inertialState = inertialState_in;
		}
		[[nodiscard]] bool getEdge() const
		{
			return this->edge;
		}
		void setEdge(bool edge_in)
		{
			this->edge = edge_in;
		}
		[[nodiscard]] double getAzSigma() const
		{
			return this->azSigma;
		}
		void setAzSigma(double azSigma_in)
		{
			this->azSigma = azSigma_in;
		}
		[[nodiscard]] double getElSigma() const
		{
			return this->elSigma;
		}
		void setElSigma(double elSigma_in)
		{
			this->elSigma = elSigma_in;
		}
		[[nodiscard]] double getBackgroundNormalizer() const
		{
			return this->backgroundNormalizer;
		}
		void setBackgroundNormalizer(double backgroundNormalizer_in)
		{
			this->backgroundNormalizer = backgroundNormalizer_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0}; ///< Time of validity, nanoseconds
		std::uint32_t detectionCategory{0};		///< Bitfield to identify which waveband temporal depth the detection is located
		std::uint32_t sensorIndex{0};			///< index indicating sensor position
		/// Sub-pixel column, row position of the candidate object.
		/// x = column
		/// y = row
		RowCol subpixel{};
		double intensity{0}; ///< Merged intensity of the candidate object
		/// Unit vector components of detection position in sensor-relative
		/// frame LOS, stored as (x,y,z)
		IR_Directional senRelUnit{};
		double signalToInterferenceRatio{0};										///< SIR
		double signalToNoiseRatio{0};												///< SNR
		std::array<std::array<std::int16_t, 3>, 3> candidateObjectWithBackground{}; ///< 3 by 3 of intensity around the candidate object
		double clutter{0};
		double candidateObjectQuality{0};											///< 0 to 1
		double sirDelta{0};				///< signal intensity from threshold to show sensitivity metric on candidate object
		SensorInertialState inertialState{};
		bool edge{false};				///< true if the detection resides near an obscuration
		double azSigma{0};				///< uncertainty in azimuth measurement
		double elSigma{0};				///< uncertainty in elevation measurement
		double backgroundNormalizer{0}; ///< background noise statistic used to calculate SIR
	};
} // namespace ams::iface::irmel
