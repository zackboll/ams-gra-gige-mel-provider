//===============================================================================
/// @file  IRSTScanTask.h
/// @brief This file includes the IRSTScanTask class.

#include <irmel/library/irmel-types/ScanParam.h>
#include <irmel/library/irmel-types/TaskInfo.h>
#include <cstdint>

#pragma once

namespace ams::iface::irmel
{
	/// @class IRSTScanTask
	/// @brief IRSTScanTask class is sent to the MFA during TaskScheduling mode,
	/// within the Operational State schedules an IRST Scan.
	/// @RequiredIfIRSTScanTasking This class provides data definition in support of IRST Scan Tasking
	/// and must be included as-is in all IR MEL implementations for IR MFAs that support scheduling of
	/// IRST scan tasks.
	class IRSTScanTask
	{
	public:
		IRSTScanTask() = default;
		IRSTScanTask(std::uint32_t id, ScanParam sp, TaskInfo ti) : commandID{id}, params{sp}, task{ti}
		{
		}
		~IRSTScanTask() = default;
		IRSTScanTask(const IRSTScanTask&) = default;
		IRSTScanTask(IRSTScanTask&&) = default;
		IRSTScanTask& operator=(const IRSTScanTask&) = default;
		IRSTScanTask& operator=(IRSTScanTask&&) = default;

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
		[[nodiscard]] ScanParam getParams() const
		{
			return this->params;
		}
		void setParams(ScanParam newParams)
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
		ScanParam params;			///< The scan-specific attributes of the task.
		TaskInfo task;				///< The general attributes of the task.
	};
} // namespace ams::iface::irmel
