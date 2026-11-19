//===============================================================================
/// @file  IRSTStareTask.h
/// @brief This file includes the IRSTStareTask class.

#include <irmel/library/irmel-types/StareTaskParam.h>
#include <irmel/library/irmel-types/TaskInfo.h>
#include <cstdint>

#pragma once

namespace ams::iface::irmel
{
	/// @class IRSTStareTask
	/// @brief IRSTStareTask class is sent to the MFA during TaskScheduling mode,
	/// within the Operational State and schedules an IRST stare.
	/// @RequiredIfIRSTStareTasking This class provides data definition in support of IRST Stare Tasking
	/// and must be included as-is in all IR MEL implementations for IR MFAs that support scheduling of
	/// IRST stare tasks.
	class IRSTStareTask
	{
	public:
		IRSTStareTask() = default;
		IRSTStareTask(std::uint32_t id, StareTaskParam sp, TaskInfo ti) : commandID{id}, params{sp}, task{ti}
		{
		}
		~IRSTStareTask() = default;
		IRSTStareTask(const IRSTStareTask&) = default;
		IRSTStareTask(IRSTStareTask&&) = default;
		IRSTStareTask& operator=(const IRSTStareTask&) = default;
		IRSTStareTask& operator=(IRSTStareTask&&) = default;

		/// @brief Gets the ID of the Task Command
		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		/// @brief Sets the ID of the Task Command
		void setCommandID(std::uint32_t newCommandID)
		{
			this->commandID = newCommandID;
		}
		[[nodiscard]] StareTaskParam getParams() const
		{
			return this->params;
		}
		void setTaskParam(StareTaskParam newParams)
		{
			this->params = newParams;
		}
		[[nodiscard]] TaskInfo getTaskInfo() const
		{
			return this->task;
		}
		void setTaskInfo(TaskInfo newTask)
		{
			this->task = newTask;
		}

	private:
		std::uint32_t commandID{0}; ///< The ID of the task command, which can be correlated with CommandStatus.
		StareTaskParam params;		///< The stare-specific attributes of the task.
		TaskInfo task;				///< The general attributes of the task.
	};
} // namespace ams::iface::irmel
