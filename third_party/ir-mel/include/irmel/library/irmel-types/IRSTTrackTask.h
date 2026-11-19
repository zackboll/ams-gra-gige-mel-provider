//===============================================================================
/// @file  IRSTTrackTask.h
/// @brief This file includes the IRSTTrackTask class.

#include <irmel/library/irmel-types/TaskInfo.h>
#include <irmel/library/irmel-types/TrackTaskParam.h>
#include <cstdint>

#pragma once

namespace ams::iface::irmel
{
	/// @class IRSTTrackTask
	/// @brief IRSTTrackTask class is sent to the MFA during TaskScheduling mode,
	/// within the Operational State and executes an schedules an IRST track.
	/// @RequiredIfIRSTTrackTasking This class provides data definition in support of IRST Track Tasking
	/// and must be included as-is in all IR MEL implementations for IR MFAs that support scheduling of
	/// IRST track tasks.
	class IRSTTrackTask
	{
	public:
		IRSTTrackTask() = default;
		IRSTTrackTask(std::uint32_t id, TrackTaskParam tp, TaskInfo ti) : commandID{id}, params{tp}, task{ti}
		{
		}
		~IRSTTrackTask() = default;
		IRSTTrackTask(const IRSTTrackTask&) = default;
		IRSTTrackTask(IRSTTrackTask&&) = default;
		IRSTTrackTask& operator=(const IRSTTrackTask&) = default;
		IRSTTrackTask& operator=(IRSTTrackTask&&) = default;

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
		[[nodiscard]] TrackTaskParam getParams() const
		{
			return this->params;
		}
		void setParams(TrackTaskParam newParams)
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
		TrackTaskParam params;		///< The track-specific attributes of the task.
		TaskInfo task;				///< The general attributes of the task.
	};
} // namespace ams::iface::irmel
