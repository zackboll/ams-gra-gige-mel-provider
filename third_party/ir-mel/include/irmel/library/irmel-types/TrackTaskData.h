//===============================================================================
/// @file  TrackTaskData.h
/// @brief This file includes the TrackTaskData class definition.

#pragma once

#include <cstdint>
#include <math/geometry/RangeAzEl.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class TrackTaskData
	/// @brief Reports the rate at which a task is completed by the MFA
	/// @RequiredIfTrack This class provides data definition in support of conditionally required IR MRL functionality to report track tasks
	/// and must be included as-is in all IR MEL implementations that support Tracks.
	class TrackTaskData
	{
	public:
		TrackTaskData() = default;
		~TrackTaskData() = default;
		TrackTaskData(const TrackTaskData&) = default;
		TrackTaskData(TrackTaskData&&) = default;
		TrackTaskData& operator=(const TrackTaskData&) = default;
		TrackTaskData& operator=(TrackTaskData&&) = default;

		[[nodiscard]] std::int32_t getTargetID() const
		{
			return this->targetID;
		}
		void setTargetID(std::int32_t targetID_in)
		{
			this->targetID = targetID_in;
		}
		[[nodiscard]] AzEl getTargetLocation() const
		{
			return this->targetLocation;
		}
		void setTargetLocation(AzEl targetLocation_in)
		{
			this->targetLocation = targetLocation_in;
		}
		[[nodiscard]] AzEl getTargetRate() const
		{
			return this->targetRate;
		}
		void setTargetRate(AzEl targetRate_in)
		{
			this->targetRate = targetRate_in;
		}

	private:
		std::int32_t targetID{0};
		AzEl targetLocation;
		AzEl targetRate;
	};
} // namespace ams::iface::irmel
