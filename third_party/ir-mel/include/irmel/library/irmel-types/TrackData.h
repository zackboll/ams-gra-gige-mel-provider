//===============================================================================
/// @file  TrackData.h
/// @brief This file includes the data definition of a single track.
//===============================================================================
#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <math/geometry/RangeAzEl.h>
#include <chrono>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class TrackData
	/// @brief A singular track, sent as a prioritized list of tracks (see PrioritizedTrackListCmd) by Service to MFA,
	/// in Scan Volume Scheduling mode, for inclusion in the schedule.
	/// @RequiredIfTrack This class provides data definition in support of Tracking and must be included as-is in all
	/// IR MEL implementations for IR MFAs that support tracking.
	class TrackData
	{
	public:
		TrackData() = default;
		~TrackData() = default;
		TrackData(const TrackData&) = default;
		TrackData(TrackData&&) = default;
		TrackData& operator=(const TrackData&) = default;
		TrackData& operator=(TrackData&&) = default;

		/// @brief gets the timestamp of the track
		[[nodiscard]] std::chrono::nanoseconds getTimestamp() const
		{
			return this->timestamp;
		}
		/// @brief sets the timestamp of the track
		void setTimestamp(std::chrono::nanoseconds timestamp_in)
		{
			this->timestamp = timestamp_in;
		}

		/// @brief gets the Track ID
		[[nodiscard]] std::int32_t getTrackId() const
		{
			return this->trackId;
		}
		/// @brief sets the Track ID
		void setTrackId(std::int32_t trackId_in)
		{
			this->trackId = trackId_in;
		}

		/// @brief gets the track priority for IRST subsystems
		[[nodiscard]] std::uint32_t getIrstPriority() const
		{
			return this->irstPriority;
		}
		/// @brief sets the track priority for IRST subsystems
		void setIrstPriority(std::uint32_t irstPriority_in)
		{
			this->irstPriority = irstPriority_in;
		}

		/// @brief gets the track priority for future subsystems
		[[nodiscard]] std::uint32_t getReservedPriority() const
		{
			return this->reservedPriority;
		}
		/// @brief sets the track priority for future subsystems
		void setReservedPriority(std::uint32_t reservedPriority_in)
		{
			this->reservedPriority = reservedPriority_in;
		}

		/// @brief gets the track precedence for IRST subsystems
		[[nodiscard]] std::uint32_t getIrstPrecedence() const
		{
			return this->irstPrecedence;
		}
		/// @brief sets the track precedence for IRST subsystems
		void setIrstPrecedence(std::uint32_t irstPrecedence_in)
		{
			this->irstPrecedence = irstPrecedence_in;
		}

		/// @brief gets the track precedence for future subsystems
		[[nodiscard]] std::uint32_t getReservedPrecedence() const
		{
			return this->reservedPrecedence;
		}
		/// @brief sets the track precedence for future subsystems
		void setReservedPrecedence(std::uint32_t reservedPrecedence_in)
		{
			this->reservedPrecedence = reservedPrecedence_in;
		}

		/// @brief gets the skippability of the track
		[[nodiscard]] bool getIsSkippable() const
		{
			return this->isSkippable;
		}
		/// @brief sets the skippability of the track
		void setIsSkippable(bool isSkippable_in)
		{
			this->isSkippable = isSkippable_in;
		}

		/// @brief gets the maintainability of the track if it leaves the FOS
		[[nodiscard]] bool getMaintainOutsideFOS() const
		{
			return this->maintainOutsideFOS;
		}
		/// @brief sets the maintainability of the track if it leaves the FOS
		void setMaintainOutsideFOS(bool maintainOutsideFOS_in)
		{
			this->maintainOutsideFOS = maintainOutsideFOS_in;
		}

		/// @brief gets the IRST subsystem track ID
		[[nodiscard]] std::int32_t getIrstTrackId() const
		{
			return this->IrstTrackId;
		}
		/// @brief sets the IRST subsystem track ID
		void setIrstTrackId(std::int32_t IrstTrackId_in)
		{
			this->IrstTrackId = IrstTrackId_in;
		}

		/// @brief gets the track position in RangeAzEl
		[[nodiscard]] RangeAzEl getPosition() const
		{
			return this->position;
		}
		/// @brief sets the track position in RangeAzEl
		void setPosition(RangeAzEl position_in)
		{
			this->position = position_in;
		}

		/// @brief gets the track velocity in RangeAzEl
		[[nodiscard]] RangeAzEl getVelocity() const
		{
			return this->velocity;
		}
		/// @brief sets the track velocity in RangeAzEl
		void setVelocity(RangeAzEl velocity_in)
		{
			this->velocity = velocity_in;
		}

		/// @brief gets the track Task type
		[[nodiscard]] const Task& getTask() const
		{
			return this->task;
		}
		/// @brief sets the track Task type
		void setTask(const Task& task_in)
		{
			this->task = task_in;
		}
		/// @brief gets the track user command ID
		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}

	private:
		std::chrono::nanoseconds timestamp{0}; ///< trackData timestamp
		std::int32_t trackId{0};			   ///< track ID
		/// Service-reported track priority for IRST subsystems. Used by the MFA when determining the overall Scan Volume Schedule
		/// @note Lower values of irstPriority result in a higher priority to be placed in the schedule
		std::uint32_t irstPriority{0};
		/// Service-reported track priority for future subsystems. Used by the MFA when determining the overall Scan Volume Schedule
		/// @note Lower values of reservedPriority result in a higher priority to be placed in the schedule
		std::uint32_t reservedPriority{0};
		/// Service-reported track precedence (within priority) for IRST subsystems. Used to sort tracks of the same priority level
		/// when determining the overall Scan Volume Schedule
		/// @note For two tracks of the same priority, the one with lower precedence will have a higher priority to be placed in the schedule
		std::uint32_t irstPrecedence{0};
		/// Service-reported track precedence (within priority) for future subsystems. Used to sort tracks of the same priority level
		/// when determining the overall Scan Volume Schedule
		/// @note For two tracks of the same priority, the one with lower precedence will have a higher priority to be placed in the schedule
		std::uint32_t reservedPrecedence{0};
		/// Indicates that the submitted track does not require scheduling during the next available scan
		bool isSkippable{false};
		/// Indicates the MFA should maintain the track even if it leaves the field of scan (FOS)
		bool maintainOutsideFOS{false}; // Request that the tracks maintained even if it leaves the FOS
		std::uint32_t commandID{0};
		std::int32_t IrstTrackId{0}; ///< IRST subsystem track ID
		RangeAzEl position;			 ///< Az/El inertial position of the target
		RangeAzEl velocity;			 ///< Az/El inertial velocity of the target
		Task task{Task::NoTask};	 ///< Output from TaskId
	};
} // end namespace ams::iface::irmel
