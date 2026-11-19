//===============================================================================
/// @file  TrackTaskParam.h
/// @brief This file includes the GRA functionality for the Instrumentation IR MEL

#pragma once
#include <irmel/library/irmel-types/CandidateObject.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/TrackListResponse.h>
#include <irmel/library/irmel-types/TrackTaskData.h>
#include <array>
#include <chrono>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
/// Of interest is the Command Type enum class,
/// which documents all commands that can be sent from the MFP to the MFA.
/// The Classes in the file
/// describe how the connections between the MFA and MFP are made.
namespace ams::iface::irmel
{
	static constexpr std::uint32_t MAX_TRACKS = 255; ///< Opportunity: Pull from config file.

	/// @class TrackTaskParam
	/// @brief Parameters needed to command a Track task
	/// @RequiredIfBuiltInTracker This class provides data definition in support of required IR MEL functionality to indicate track task parameters
	/// and must be included as-is in all IR MEL implementations
	class TrackTaskParam
	{
	public:
		TrackTaskParam() = default;
		~TrackTaskParam() = default;
		TrackTaskParam(const TrackTaskParam&) = default;
		TrackTaskParam(TrackTaskParam&&) = default;
		TrackTaskParam& operator=(const TrackTaskParam&) = default;
		TrackTaskParam& operator=(TrackTaskParam&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getDurationNanoseconds() const
		{
			return this->durationNanoseconds;
		}
		void setDurationNanoseconds(std::chrono::nanoseconds durationNanoseconds_in)
		{
			this->durationNanoseconds = durationNanoseconds_in;
		}
		[[nodiscard]] std::uint32_t getCount() const
		{
			return this->count;
		}
		void setCount(std::uint32_t count_in)
		{
			this->count = count_in;
		}
		[[nodiscard]] const std::array<TrackTaskData, MAX_TRACKS>& getTargetData() const
		{
			return this->targetData;
		}
		void setTargetData(const std::array<TrackTaskData, MAX_TRACKS>& targetData_in)
		{
			this->targetData = targetData_in;
		}

	private:
		std::uint32_t count{0};							 ///< num targets
		std::chrono::nanoseconds durationNanoseconds{0}; ///< total duration
		std::array<TrackTaskData, MAX_TRACKS> targetData;
	};

} // end namespace ams::iface::irmel
