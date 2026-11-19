//===============================================================================
/// @file  PrioritizedTrackListCmd.h
/// @brief This file includes the PrioritizedTrackListCmd class definition.

#pragma once

#include <irmel/library/irmel-types/TrackData.h>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class PrioritizedTrackListCmd
	/// @brief The MFA sends a command to create a list to track the priority of tasks
	/// @RequiredIfScanVolumeSchedulingOnMFA This class provides data definition in support of required Ir MEL functionality to send commands to
	/// create a prioritized track list of task and must be included as-is in all Ir MEL implementations for MFAs that support ScanVolumeSchedule.
	class PrioritizedTrackListCmd
	{
	public:
		PrioritizedTrackListCmd() = default;
		PrioritizedTrackListCmd(std::chrono::nanoseconds sch, std::chrono::nanoseconds live, std::uint32_t cmdId, std::uint32_t cnt,
								std::vector<TrackData> trk)
			: scheduleTime{sch}, timeToLive{live}, commandID{cmdId}, trackCount{cnt}, trackData{std::move(trk)}
		{
		}

		~PrioritizedTrackListCmd() = default;
		PrioritizedTrackListCmd(const PrioritizedTrackListCmd&) = default;
		PrioritizedTrackListCmd(PrioritizedTrackListCmd&&) = default;
		PrioritizedTrackListCmd& operator=(const PrioritizedTrackListCmd&) = default;
		PrioritizedTrackListCmd& operator=(PrioritizedTrackListCmd&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getScheduleTime() const
		{
			return this->scheduleTime;
		}
		void setScheduleTime(std::chrono::nanoseconds st)
		{
			this->scheduleTime = st;
		}
		[[nodiscard]] std::chrono::nanoseconds getTimeToLive() const
		{
			return this->timeToLive;
		}
		void setTimeToLive(std::chrono::nanoseconds ttl)
		{
			this->timeToLive = ttl;
		}
		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::uint32_t getTrackCount() const
		{
			return this->trackCount;
		}
		void setTrackCount(std::uint32_t tc)
		{
			this->trackCount = tc;
		}
		[[nodiscard]] const std::vector<TrackData>& getTrackData() const
		{
			return this->trackData;
		}
		// replace the existing vector with a new vector
		void setTrackData(const std::vector<TrackData>& data)
		{
			this->trackData = data;
		}
		// add a new element to the vector
		void addTrackData(const TrackData& data)
		{
			this->trackData.push_back(data);
		}

	private:
		std::chrono::nanoseconds scheduleTime{0};
		std::chrono::nanoseconds timeToLive{0}; ///< Duration before expiring these tracks
		std::uint32_t commandID{0};
		std::uint32_t trackCount{0};
		std::vector<TrackData> trackData{};
	};
} // end namespace ams::iface::irmel
