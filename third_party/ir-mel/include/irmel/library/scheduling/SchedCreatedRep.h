//===============================================================================
/// @file  SchedCreatedRep.h
/// @brief This file includes the data definition for the schedule created message as
/// reported by the MFA.

#pragma once

#include <irmel/library/scheduling/ActivityRecord.h>
#include <cstdint>
#include <chrono>
#include <utility>
#include <vector>

/// @namespace ams::iface::irmel
/// This namespace contains the SchedCreatedRep class definition
namespace ams::iface::irmel
{
	/// @class SchedCreatedRep
	/// @brief The SchedCreatedRep class creates a schedule of activity being performed as a report
	/// @RequiredIfScanVolumeSchedulingOnMFA This class provides data definitions in support of IR MEL functionality to create a scheduling report
	/// of activity records and must be included as-is in all IR MEL implementations for MFAs that support ScanVolumeSchedule
	class SchedCreatedRep
	{
	public:
		SchedCreatedRep() = default;
		SchedCreatedRep(std::uint32_t cmdId, std::uint32_t id, std::chrono::nanoseconds dur, std::uint32_t cnt, std::vector<ActivityRecord> act)
			: commandID{cmdId}, scanId{id}, estDuration{dur}, activityCount{cnt}, activity{std::move(act)}
		{
		}
		~SchedCreatedRep() = default;
		SchedCreatedRep(const SchedCreatedRep&) = default;
		SchedCreatedRep(SchedCreatedRep&&) = default;
		SchedCreatedRep& operator=(const SchedCreatedRep&) = default;
		SchedCreatedRep& operator=(SchedCreatedRep&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::uint32_t getScanId() const
		{
			return this->scanId;
		}
		void setScanId(std::uint32_t id)
		{
			this->scanId = id;
		}
		[[nodiscard]] std::chrono::nanoseconds getEstDuration() const
		{
			return this->estDuration;
		}
		void setEstDuration(std::chrono::nanoseconds est)
		{
			this->estDuration = est;
		}
		[[nodiscard]] std::uint32_t getActivityCount() const
		{
			return this->activityCount;
		}
		void setActivityCount(std::uint32_t count)
		{
			this->activityCount = count;
		}
		[[nodiscard]] const std::vector<ActivityRecord>& getActivity() const
		{
			return this->activity;
		}
		// replace the existing vector with a new vector
		void setActivity(const std::vector<ActivityRecord>& act)
		{
			this->activity = act;
		}
		// add a new element to the vector
		void addActivityRecord(const ActivityRecord& act)
		{
			this->activity.push_back(act);
		}

	private:
		std::uint32_t commandID{0};
		std::uint32_t scanId{0};
		std::chrono::nanoseconds estDuration{0};
		std::uint32_t activityCount{0};
		std::vector<ActivityRecord> activity{};
	};
} // namespace ams::iface::irmel
