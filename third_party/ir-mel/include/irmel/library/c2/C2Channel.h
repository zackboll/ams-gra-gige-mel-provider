//===============================================================================
/// @file  C2Channel.h
/// @brief This file includes the C2 Channel type.
//===============================================================================
#pragma once

#include <irmel/library/c2/BIT_Command.h>
#include <irmel/library/c2/CalibrationConfigurationCmd.h>
#include <irmel/library/c2/CalibrationStatusCmd.h>
#include <irmel/library/c2/CameraProtectCmd.h>
#include <irmel/library/c2/ConfigSetCommand.h>
#include <irmel/library/c2/EraseCommand.h>
#include <irmel/library/c2/EraseCommandType.h>
#include <irmel/library/c2/ModeCmd.h>
#include <irmel/library/irmel-types/CameraCommand.h>
#include <irmel/library/irmel-types/CameraCommandResp.h>
#include <irmel/library/irmel-types/Channel.h>
#include <irmel/library/irmel-types/CommandStatus.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/RequestSystemTrackData.h>
#include <irmel/library/irmel-types/SystemTrackDataResponse.h>
#include <mel/library/CommonMEL.h>
#include <functional>

namespace ams::iface::irmel
{
	/// @class C2Channel
	/// @brief Instrumentation Channel used by services.
	/// @Required This class provides required IR MEL functionality for reporting channel details, and must be provided by the implementer
	/// in all IR MEL implementations. See individual members for details
	class C2Channel : public virtual Channel
	{
	public:
		/// @Required This function supports channel and must be provided by the implementer for all IR MEL implementations
		~C2Channel() override = default;

		/// @brief Returns after sending BIT_Command
		/// @param[in] bit_Command - Command to send
		/// @Required These functions allow for commands to be sent and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<Return> send(BIT_Command bit_Command) = 0;

		/// @brief Returns CalibrationConfiguration after sending CalibrationConfigurationCmd
		/// @param[in] calibrationConfigurationCmd - Command to send
		/// @Optional This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<ams::iface::mel::CalibrationConfiguration> send(
			CalibrationConfigurationCmd calibrationConfigurationCmd) = 0;

		/// @brief Returns CalibrationStatus after sending CalibrationStatusCmd
		/// @param[in] calibrationStatusCmd - Command to send
		/// @Optional This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<ams::iface::mel::CalibrationStatus> send(CalibrationStatusCmd calibrationStatusCmd) = 0;

		/// @brief Returns CameraCommandRespafter sending CameraCommand
		/// @param[in] cameraCommand - Command to send
		/// @RequiredIfCameraCtrl This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL
		/// implementations
		virtual ams::iface::mel::RequestFor<CameraCommandResp> send(CameraCommand cameraCommand) = 0;

		/// @brief Returns CameraProtectCmdResp after sending CameraProtectCmd
		/// @param[in] cameraProtectCmd - Command to send
		/// @RequiredIfCameraProtect This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL
		/// implementations
		virtual ams::iface::mel::RequestFor<CameraProtectCmdResp> send(CameraProtectCmd cameraProtectCmd) = 0;

		/// @brief Returns EraseCommandType after sending EraseCommand
		/// @param[in] eraseCommand - Command to send
		/// @Optional This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<EraseCommandType> send(EraseCommand eraseCommand) = 0;

		/// @brief Returns CommandStatus after sending SystemTrackDataResponse
		/// @param[in] systemTrackDataResponse - Command to send
		/// @Optional This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<CommandStatus> send(SystemTrackDataResponse systemTrackDataResponse) = 0;

		/// @brief Returns MFA_Mode after sending ModeCmd
		/// @param[in] modeCmd - Command to send
		/// @Required This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<MFA_Mode> send(ModeCmd modeCmd) = 0;

		/// @brief Returns after sending ConfigSetCommand
		/// @param[in] config - Command to send
		/// @Required This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<Return> send(ConfigSetCommand config) = 0;

		/// @brief Callback Registration for BIT_Configuration
		/// @param callback std::function callback for BIT_Configuration
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ams::iface::mel::BIT_Configuration* const)> callback) = 0;

		/// @brief Callback Registration for CommandStatus
		/// @param callback std::function callback for CommandStatus
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const CommandStatus* const)> callback) = 0;

		/// @brief Callback Registration for CalibrationConfiguration
		/// @param callback std::function callback for CalibrationConfiguration
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration in support of IR MEL functionality to receive the Calibration
		/// Configuration of the MFA.
		virtual Return registerMetadataCallback(
			std::function<void(Channel& channel, const ams::iface::mel::CalibrationConfiguration* const)> callback) = 0;

		/// @brief Callback Registration for CameraProtectCmdResp
		/// @param callback std::function callback for CameraProtectCmdResp
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @RequiredIfCameraProtect This class provides data definition in support of Camera Self-Protection
		/// and must be included as-is in all IR MEL implementations for IR MFAs that support camera self-protection
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const CameraProtectCmdResp* const)> callback) = 0;

		/// @brief Callback Registration for CalibrationStatus
		/// @param callback std::function callback for CalibrationStatus
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration in support of IR MEL functionality to receive the Calibration
		/// Status of the MFA.
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ams::iface::mel::CalibrationStatus* const)> callback) = 0;

		/// @brief Callback Registration for BIT_Status
		/// @param callback std::function callback for BIT_Status
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ams::iface::mel::BIT_Status* const)> callback) = 0;

		using Channel::registerMetadataCallback;
		using Channel::send;

		C2Channel() = default;
		C2Channel(const C2Channel&) = delete;
		C2Channel& operator=(C2Channel&) = delete;
		C2Channel(C2Channel&& other) = delete;
		C2Channel& operator=(C2Channel&& other) = delete;
	};
	// end interface C2Channel

} // end namespace ams::iface::irmel
