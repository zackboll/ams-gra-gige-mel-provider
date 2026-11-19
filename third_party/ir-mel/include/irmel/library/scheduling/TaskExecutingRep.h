//===============================================================================
/// @file  TaskExecutingRep.h
/// @brief This file includes the data definition of the task executing report as
/// reported by the MFA.

#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <cstdint>
#include <chrono>

/// @namespace ams::iface::irmel
/// This namespace contains the TaskExecutingRep class definition
namespace ams::iface::irmel
{
	/// @class TaskExecutingRep
	/// @brief TaskExecutingRep is used for the MFA to send reports to
	/// the MFP on the currently executing Task.
	/// @RequiredIfIRSTTrackTasking This class provides data definition in support of conditionally required IR MEL functionality to send reports on
	/// executing Tasks and must be included as-is in all IR MEL implementations that support IRST Track Tasking.
	class TaskExecutingRep
	{
	public:
		TaskExecutingRep() = default;
		~TaskExecutingRep() = default;
		TaskExecutingRep(const TaskExecutingRep&) = default;
		TaskExecutingRep(TaskExecutingRep&&) = default;
		TaskExecutingRep& operator=(const TaskExecutingRep&) = default;
		TaskExecutingRep& operator=(TaskExecutingRep&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::uint32_t getActivityId() const
		{
			return this->activityId;
		}
		void setActivityId(std::uint32_t activityId_in)
		{
			this->activityId = activityId_in;
		}
		[[nodiscard]] std::uint32_t getReqId() const
		{
			return this->reqId;
		}
		void setReqId(std::uint32_t reqId_in)
		{
			this->reqId = reqId_in;
		}
		[[nodiscard]] const Task& getTask() const
		{
			return this->task;
		}
		void setTask(Task task_in)
		{
			this->task = task_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getEstimatedDuration() const
		{
			return this->estimatedDuration;
		}
		void setEstimatedDuration(std::chrono::nanoseconds estimatedDuration_in)
		{
			this->estimatedDuration = estimatedDuration_in;
		}
		[[nodiscard]] std::int32_t getTrackId() const
		{
			return this->trackId;
		}
		void setTrackId(std::int32_t trackId_in)
		{
			this->trackId = trackId_in;
		}
		[[nodiscard]] std::uint32_t getPrevActivityId() const
		{
			return this->prevActivityId;
		}
		void setPrevActivityId(std::uint32_t prevActivityId_in)
		{
			this->prevActivityId = prevActivityId_in;
		}
		[[nodiscard]] std::uint32_t getPrevReqId() const
		{
			return this->prevReqId;
		}
		void setPrevReqId(std::uint32_t prevReqId_in)
		{
			this->prevReqId = prevReqId_in;
		}
		[[nodiscard]] const Task& getPrevTask() const
		{
			return this->prevTask;
		}
		void setPrevTask(Task prevTask_in)
		{
			this->prevTask = prevTask_in;
		}
		[[nodiscard]] TaskCompStatus getPrevTaskStatus() const
		{
			return this->prevTaskStatus;
		}
		void setPrevTaskStatus(TaskCompStatus prevTaskStatus_in)
		{
			this->prevTaskStatus = prevTaskStatus_in;
		}

	private:
		std::uint32_t activityId{0};
		std::uint32_t commandID{0};
		std::uint32_t reqId{0};
		Task task{Task::NoTask};
		std::chrono::nanoseconds estimatedDuration{0};
		std::int32_t trackId{0};
		std::uint32_t prevActivityId{0};
		std::uint32_t prevReqId{0};
		Task prevTask{Task::NoTask};
		TaskCompStatus prevTaskStatus{TaskCompStatus::NoTask};
	};
} // namespace ams::iface::irmel
