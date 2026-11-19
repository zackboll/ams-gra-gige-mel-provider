//===============================================================================
/// @file  AbortTask.h
/// @brief This file includes the AbortTask class.

#include <irmel/library/irmel-types/TaskInfo.h>

#pragma once

namespace ams::iface::irmel
{
	/// @class AbortTask
	/// @brief AbortTask class is sent to the MFA during TaskScheduling mode,
	/// within the Operational State and aborts currently executing task.
	/// @Required This class provides data definition in support of required IR MEL functionality to indicate task command execution
	/// and must be included as-is in all IR MEL implementation
	class AbortTask
	{
	public:
		AbortTask() = default;
		explicit AbortTask(TaskInfo ti) : task{ti}
		{
		}
		~AbortTask() = default;
		AbortTask(const AbortTask&) = default;
		AbortTask(AbortTask&&) = default;
		AbortTask& operator=(const AbortTask&) = default;
		AbortTask& operator=(AbortTask&&) = default;

	private:
		TaskInfo task;
	};
} // namespace ams::iface::irmel
