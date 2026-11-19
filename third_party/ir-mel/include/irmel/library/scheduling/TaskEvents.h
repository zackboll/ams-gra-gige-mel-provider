//===============================================================================
/// @file  TaskEvents.h
/// @brief This file includes the TaskEvent class and EventType enumeration

#pragma once

#include <chrono>
#include <irmel/library/scheduling/EventType.h>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class TaskEvents
	/// @brief Provide information for task events
	/// @Optional This class provides a data definition in support of task events
	class TaskEvents
	{
	public:
		TaskEvents() = default;
		~TaskEvents() = default;
		TaskEvents(const TaskEvents&) = default;
		TaskEvents(TaskEvents&&) = default;
		TaskEvents& operator=(const TaskEvents&) = default;
		TaskEvents& operator=(TaskEvents&&) = default;

		[[nodiscard]] const EventType& getEventType() const
		{
			return this->eventType;
		}
		void setEventType(EventType eventType_in)
		{
			this->eventType = eventType_in;
		}
		[[nodiscard]] std::uint32_t getScanBar() const
		{
			return this->scanBar;
		}
		void setScanBar(std::uint32_t scanBar_in)
		{
			this->scanBar = scanBar_in;
		}
		[[nodiscard]] std::uint32_t getStopNumber() const
		{
			return this->stopNumber;
		}
		void setStopNumber(std::uint32_t stopNumber_in)
		{
			this->stopNumber = stopNumber_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
		}
		[[nodiscard]] std::uint32_t getTrackId() const
		{
			return this->trackId;
		}
		void setTrackId(std::uint32_t trackId_in)
		{
			this->trackId = trackId_in;
		}

	private:
		EventType eventType{EventType::enumNotSet}; ///< Specifies the type of event
		std::uint32_t scanBar{0};					///< indicates scan bar
		std::uint32_t stopNumber{0};				///< Specifies stop number
		std::chrono::nanoseconds systemTime{0};		///< System time for this command, in nanoseconds
		std::uint32_t trackId{0};					///< Specifies the track ID
	};
} // end namespace ams::iface::irmel
