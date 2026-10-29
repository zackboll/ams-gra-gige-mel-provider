//===============================================================================
/// @file  ImageChannel.h
/// @brief This file includes the Image Channel type.
//===============================================================================
#pragma once

#include <irmel/library/image/NavigationReportResp.h>
#include <irmel/library/irmel-types/BadPixelList.h>
#include <irmel/library/irmel-types/CameraCommand.h>
#include <irmel/library/irmel-types/CameraCommandResp.h>
#include <irmel/library/irmel-types/CandidateObjectMessage.h>
#include <irmel/library/irmel-types/CandidateObjectPreProc.h>
#include <irmel/library/irmel-types/CandidateObjectPreProcMessage.h>
#include <irmel/library/irmel-types/Channel.h>
#include <irmel/library/irmel-types/LOS3D_KinematicsType.h>
#include <irmel/library/irmel-types/LineOfSightEuler.h>
#include <irmel/library/irmel-types/LineOfSightQuaternion.h>
#include <irmel/library/irmel-types/LineOfSightReport.h>
#include <irmel/library/irmel-types/NUC_TempData.h>
#include <irmel/library/irmel-types/OpticalDistortionMap.h>
#include <mel/library/CommonMEL.h>
#include <functional>

namespace ams::iface::irmel
{
	/// @class ImageChannel
	/// @brief Image Channel used by services.
	/// @Required This class provides required IR MEL functionality for reporting channel details, and must be provided by the implementer
	/// in all IR MEL implementations. See individual members for details
	class ImageChannel : public virtual Channel
	{
	public:
		/// @Required This function supports channel and must be provided by the implementer for all IR MEL implementations
		~ImageChannel() override = default;

		/// @brief Returns CameraCommandResp after sending CameraCommand
		/// @param[in] cameraCommand - Command to send
		/// @RequiredIfCameraCtrl This function provides data definition in support of Camera Control
		/// and must be included as-is in all IR MEL implementations for IR MFAs that support programmable control of camera
		virtual ams::iface::mel::RequestFor<CameraCommandResp> send(CameraCommand cameraCommand) = 0;

		/// @brief Returns NavigationReportResp after sending NavigationReport
		/// @param[in] navigationReport - Command to send
		/// @Required These function allow for the specific commands to be sent and must be provided by the implementer for all IR MEL
		/// implementations
		virtual ams::iface::mel::RequestFor<NavigationReportResp> send(NavigationReport navigationReport) = 0;

		/// @brief Callback Registration for BadPixelList
		/// @param callback std::function callback for BadPixelList
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///    callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const BadPixelList* const)> callback) = 0;

		/// @brief Callback Registration for OpticalDistortionMap
		/// @param callback std::function callback for OpticalDistortionMap
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///    callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration for IR MEL implementation is optional.
		/// for MFAs that provide additional image metadata to support image processing performed by a service
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const OpticalDistortionMap* const)> callback) = 0;

		/// @brief Callback Registration for LineOfSightReport
		/// @param callback std::function callback for LineOfSightReport
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///    callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const LineOfSightReport* const)> callback) = 0;

		/// @brief Callback Registration for LineOfSightQuaternion
		/// @param callback std::function callback for LineOfSightQuaternion
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///    callback is already registered for this datatype on this channel
		/// @RequiredIfLOSQuaternion This function supports callback registration and must be provided by the implementer
		/// if the MFA Provider supports LOS in Quaternion
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const LineOfSightQuaternion* const)> callback) = 0;

		/// @brief Callback Registration for LineOfSightEuler
		/// @param callback std::function callback for LineOfSightEuler
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///    callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const LineOfSightEuler* const)> callback) = 0;

		/// @brief Callback Registration for CameraCommandResp
		/// @param callback std::function callback for CameraCommandResp
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @RequiredIfCameraCtrl This function supports callback registration and must be provided by the implementer
		/// if the MFA Provider supports programmable camera control
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const CameraCommandResp* const)> callback) = 0;

		/// @brief Callback Registration for NavigationReportResp
		/// @param callback std::function callback for NavigationReportResp
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const NavigationReportResp* const)> callback) = 0;

		/// @brief Callback Registration for LOS3D_Kinematics
		/// @param callback std::function callback for LOS3D_Kinematics
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration and is optional for IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const LOS3D_KinematicsType* const)> const& callback) = 0;

		/// @brief Callback Registration for CandidateObjectMessage
		/// @param callback std::function callback for CandidateObjectMessage
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///      callback is already registered for this datatype on this channel
		/// @RequiredIfDetectCandidateObjects This function supports callback registration and must be provided by the implementer
		/// if the MFA provides Candidate Object detections
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const CandidateObjectMessage* const)> callback) = 0;

		/// @brief Callback Registration for CandidateObjectPreProcMessage
		/// @param callback std::function callback for CandidateObjectPreProcMessage
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///    callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration and IR MEL implementation is optional.
		/// Intended for use by IR MFAs that use CandidateObjectPreProc
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const CandidateObjectPreProcMessage* const)> callback) = 0;

		/// @brief Callback Registration for NUC_TempData
		/// @param callback std::function callback for NUC_TempData
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///    callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration and IR MEL implementation is optional.
		/// Intended for use by IR MFAs that perform NUC calibration
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const NUC_TempData* const)> callback) = 0;

		using Channel::registerMetadataCallback;
		using Channel::send;

		ImageChannel() = default;
		ImageChannel(const ImageChannel&) = delete;
		ImageChannel& operator=(ImageChannel&) = delete;
		ImageChannel(ImageChannel&& other) = delete;
		ImageChannel& operator=(ImageChannel&& other) = delete;
	};
	// end interface ImageChannel

} // end namespace ams::iface::irmel
