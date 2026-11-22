//===============================================================================
/// @file  ActivityRecord.h
/// @brief This file includes the definition of the ActivityRecord class.

#pragma once

#include <cstdint>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <math/geometry/RangeAzEl.h>
#include <chrono>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class ActivityRecord
	/// @brief Keeps a record of the the activity that is performed by a task.
	/// @RequiredIfIRSTTrackTasking This class provides data definition in support of conditionally required IR MEL functionality to record
	/// task activity and must be included as-is in all IR MEL implementations that support IRST Track Tasking.
	class ActivityRecord
	{
	public:
		ActivityRecord() = default;
		ActivityRecord(std::uint32_t act, Task tsk, std::int32_t track, RangeAzEl pos, std::chrono::nanoseconds dur)
			: activityId{act}, taskId{tsk}, trackId{track}, position{pos}, duration{dur}
		{
		}
		~ActivityRecord() = default;
		ActivityRecord(const ActivityRecord&) = default;
		ActivityRecord(ActivityRecord&&) = default;
		ActivityRecord& operator=(const ActivityRecord&) = default;
		ActivityRecord& operator=(ActivityRecord&&) = default;

		[[nodiscard]] std::uint32_t getActivityId() const
		{
			return this->activityId;
		}
		void setActivityId(std::uint32_t activityId_in)
		{
			this->activityId = activityId_in;
		}
		[[nodiscard]] const Task& getTaskId() const
		{
			return this->taskId;
		}
		void setTaskId(Task taskId_in)
		{
			this->taskId = taskId_in;
		}
		[[nodiscard]] std::int32_t getTrackId() const
		{
			return this->trackId;
		}
		void setTrackId(std::int32_t trackId_in)
		{
			this->trackId = trackId_in;
		}
		[[nodiscard]] RangeAzEl getPosition() const
		{
			return this->position;
		}
		void setPosition(RangeAzEl position_in)
		{
			this->position = position_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getDuration() const
		{
			return this->duration;
		}
		void setDuration(std::chrono::nanoseconds duration_in)
		{
			this->duration = duration_in;
		}

	private:
		std::uint32_t activityId{0};
		Task taskId{0};
		std::int32_t trackId{0};
		RangeAzEl position;
		std::chrono::nanoseconds duration{0};
	};
} // namespace ams::iface::irmel
