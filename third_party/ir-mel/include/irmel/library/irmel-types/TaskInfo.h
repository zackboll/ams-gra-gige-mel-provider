//===============================================================================
/// @file  TaskInfo.h
/// @brief This file includes the data definition of the Execute Task Command.
//===============================================================================
#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <math/geometry/RangeAzEl.h>
#include <chrono>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class Task
	/// @brief Base class for tasks sent to the MFA during
	/// TaskScheduling mode, within the Operational State.
	/// @Required This class provides data definition in support of required IR MEL functionality to indicate task command execution
	/// and must be included as-is in all IR MEL implementation
	class TaskInfo
	{
	public:
		TaskInfo() = default;
		~TaskInfo() = default;
		TaskInfo(const TaskInfo&) = default;
		TaskInfo(TaskInfo&&) = default;
		TaskInfo& operator=(const TaskInfo&) = default;
		TaskInfo& operator=(TaskInfo&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
		}
		[[nodiscard]] std::uint32_t getReqId() const
		{
			return this->reqId;
		}
		void setReqId(std::uint32_t reqId_in)
		{
			this->reqId = reqId_in;
		}
		[[nodiscard]] const TaskListAction& getAction() const
		{
			return this->action;
		}
		void setAction(const TaskListAction& action_in)
		{
			this->action = action_in;
		}
		[[nodiscard]] std::uint32_t getPrecedenceWithinPriority() const
		{
			return this->precedenceWithinPriority;
		}
		void setPrecedenceWithinPriority(std::uint32_t precedenceWithinPriority_in)
		{
			this->precedenceWithinPriority = precedenceWithinPriority_in;
		}
		[[nodiscard]] std::uint32_t getPriority() const
		{
			return this->priority;
		}
		void setPriority(std::uint32_t priority_in)
		{
			this->priority = priority_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0};
		std::uint32_t reqId{0};

		TaskListAction action{TaskListAction::Normal};
		/// @brief PrecedenceWithinPriority of this task from the OMS command, used in conjunction with Priority, to determine appropriate place in
		/// Task List. Lower values of PrecedenceWithinPriority and Priority result in a higher position on the Task List. A task with lower
		/// PrecedenceWithinPriority is placed higher in the task list than a task of higher PrecedenceWithinPriority.
		std::uint32_t precedenceWithinPriority{0};
		/// @brief Priority of this task from the OMS command, used in conjunction with Precendence, to determine appropriate place in Task List.
		/// Lower values of PrecedenceWithinPriority and Priority result in a higher position on the Task List. If two tasks contain the same
		/// PRIORITY, the one with lower PRECEDENCE will be placed higher in the task list than the one of higher PRECEDENCE. Hence the name
		/// PrecedenceWithinPriority.
		std::uint32_t priority{0};
	};

} // end namespace ams::iface::irmel
