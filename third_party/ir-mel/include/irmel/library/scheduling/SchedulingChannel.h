//===============================================================================
/// @file  SchedulingChannel.h
/// @brief This file includes the Scheduling Channel type.
//===============================================================================
#pragma once

#include <irmel/library/irmel-types/AbortTask.h>
#include <irmel/library/irmel-types/Channel.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/IRSTScanTask.h>
#include <irmel/library/irmel-types/IRSTStareTask.h>
#include <irmel/library/irmel-types/IRSTTrackTask.h>
#include <irmel/library/irmel-types/PrioritizedTrackListCmd.h>
#include <irmel/library/irmel-types/UpdateTrackListResponse.h>
#include <irmel/library/scheduling/ExecuteTaskAck.h>
#include <irmel/library/scheduling/ScanPerformanceReport.h>
#include <irmel/library/scheduling/SchedCreatedRep.h>
#include <irmel/library/scheduling/TaskEvents.h>
#include <irmel/library/scheduling/TaskExecutingRep.h>
#include <mel/library/CommonMEL.h>
#include <functional>

namespace ams::iface::irmel
{
	/// @class SchedulingChannel
	/// @brief Reports the data that as channel's image stram receives.
	/// @RequiredIfIRSTTrackTasking This class defines the conditionally required Scheduling Channel interface and must be provided by the
	/// implementer in all IR MEL implementations that support this channel. See individual members for details.
	class SchedulingChannel : public virtual Channel
	{
	public:
		using Channel::registerMetadataCallback;
		using Channel::send;

		SchedulingChannel() = default;
		SchedulingChannel(const SchedulingChannel&) = delete;
		SchedulingChannel& operator=(SchedulingChannel&) = delete;
		SchedulingChannel(SchedulingChannel&& other) = delete;
		SchedulingChannel& operator=(SchedulingChannel&& other) = delete;

		/// @Required This function supports channel and must be provided by the implementer for all IR MEL implementations
		~SchedulingChannel() override = default;

		/// @brief Returns TrackListResponse after sending PrioritizedTrackListCmd
		/// @param[in] prioritizedTrackListCmd - Command to send
		/// @Required This function supports sending channel commands and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<TrackListResponse> send(PrioritizedTrackListCmd prioritizedTrackListCmd) = 0;

		/// @brief Callback Registration for TaskExecutingRep
		/// @param callback std::function callback for TaskExecutingRep
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///         callback is already registered for this datatype on this channel
		/// @RequiredIfIRSTTrackTasking This function supports callback registration and must be provided by the implementer for all IR MEL
		/// implementations that support IRST Track Tasking.
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const TaskExecutingRep* const)> callback) = 0;

		/// @brief Callback Registration for ExecuteTaskAck
		/// @param callback std::function callback for ExecuteTaskAck
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///         callback is already registered for this datatype on this channel
		/// @RequiredIfIRSTTrackTasking This function supports callback registration and must be provided by the implementer for all IR MEL
		/// implementations that support IRST Track Tasking.
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ExecuteTaskAck* const)> callback) = 0;

		/// @brief Callback Registration for SchedCreatedRep
		/// @param callback std::function callback for SchedCreatedRep
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///         callback is already registered for this datatype on this channel
		/// @RequiredIfIRSTTrackTasking This function supports callback registration and must be provided by the implementer for all IR MEL
		/// implementations that support IRST Track Tasking.
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const SchedCreatedRep* const)> callback) = 0;

		/// @brief Callback Registration for UpdateTrackListResponse
		/// @param callback std::function callback for UpdateTrackListResponse
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///         callback is already registered for this datatype on this channel
		/// @RequiredIfTrack This function supports callback registration and must be provided by the implementer
		///                  if all IR MEL Implementations support tracking
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const UpdateTrackListResponse* const)> callback) = 0;

		/// @brief Callback Registration for TaskEvents
		/// @param callback std::function callback for TaskEvents
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Optional Implementation may be provided for MFAs that support TaskEvent metadata
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const TaskEvents* const)> callback) = 0;

		/// @brief Callback Registration for ScanPerformanceReport
		/// @param callback std::function callback for ScanPerformanceReport
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		/// callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration in support of IR MEL functionality to receive the Scan Performance
		/// Report of the executed scan.
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ScanPerformanceReport* const)> callback) = 0;

		/// @brief Method to execute an AbortTask task
		/// @param[in] abortTask - Task to abort
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and task execution scheduled, Return::Fail
		///     otherwise
		/// @RequiredIfIRSTScanTasking This function provides data definition in support executing IRST scans
		///                  and must be included as-is in all IR MEL implementations for IR MFAs that support IRST scanning
		/// @RequiredIfIRSTStareTasking This function provides data definition in support executing IRST stares
		///                  and must be included as-is in all IR MEL implementations for IR MFAs that support IRST staring
		/// @RequiredIfIRSTTrackTasking This function provides data definition in support executing IRST tracks
		///                  and must be included as-is in all IR MEL implementations for IR MFAs that support IRST trackings
		virtual ams::iface::mel::RequestFor<Return> exec(AbortTask abortTask) = 0;

		/// @brief Method to execute an IRSTScanTask task
		/// @param[in] irstScanTask - Scan task parameters
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and task execution scheduled, Return::Fail
		///     otherwise
		/// @RequiredIfIRSTScanTasking This function provides data definition in support executing IRST scans
		///                  and must be included as-is in all IR MEL implementations for IR MFAs that support IRST scanning
		virtual ams::iface::mel::RequestFor<Return> exec(IRSTScanTask irstScanTask) = 0;

		/// @brief Method to execute an IRSTStareTask task
		/// @param[in] irstStareTask - Stare task parameters
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and task execution scheduled, Return::Fail
		///     otherwise
		/// @RequiredIfIRSTStareTasking This function provides data definition in support executing IRST stares
		///                  and must be included as-is in all IR MEL implementations for IR MFAs that support IRST staring
		virtual ams::iface::mel::RequestFor<Return> exec(IRSTStareTask irstStareTask) = 0;

		/// @brief Method to execute an IRSTTrackTask task
		/// @param[in] irstTrackTask - Track task parameters
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and task execution scheduled, Return::Fail
		///     otherwise
		/// @RequiredIfIRSTTrackTasking This function provides data definition in support executing IRST tracks
		///                  and must be included as-is in all IR MEL implementations for IR MFAs that support IRST trackings
		virtual ams::iface::mel::RequestFor<Return> exec(IRSTTrackTask irstTrackTask) = 0;
	};
	// end interface SchedulingChannel

} // end namespace ams::iface::irmel
