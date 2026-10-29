//===============================================================================
/// @file  HealthStatusChannel.h
/// @brief This file includes the Health and Status Channel type.
//===============================================================================
#pragma once

#include <irmel/library/health-status/LFStatus.h>
#include <irmel/library/health-status/SubsystemStatusResp.h>
#include <irmel/library/irmel-types/Channel.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/NUC_TempData.h>
#include <mel/library/CommonMEL.h>
#include <mel/library/MFA_StatusDetailed.h>
#include <mel/library/securityauditrecord/MFA_SecurityAuditRecord.h>
#include <functional>

namespace ams::iface::irmel
{
	/// @class HealthStatusChannel
	/// @brief Health and Status Channel used by services.
	/// @Required This class provides required IR MEL functionality for reporting channel details, and must be provided by the implementer
	/// in all IR MEL implementations. See individual members for details
	class HealthStatusChannel : public virtual Channel
	{
	public:
		/// @Required This function supports channel and must be provided by the implementer for all IR MEL implementations
		~HealthStatusChannel() override = default;

		/// @brief Callback Registration for LFStatus
		/// @param callback std::function callback for LFStatus
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @RequiredIfLFSupport This function supports callback registration for LFStatus and must be included as-is in all IR MEL
		/// implementations for IR MFAs that support local functions.
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const LFStatus* const)> callback) = 0;

		/// @brief Callback Registration for MFA_Status
		/// @param callback std::function callback for MFA_Status
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ams::iface::mel::MFA_Status* const)> callback) = 0;

		/// @brief Callback Registration for BIT_Status
		/// @param callback std::function callback for BIT_Status
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ams::iface::mel::BIT_Status* const)> callback) = 0;

		/// @brief Callback Registration for SubsystemStatusResp
		/// @param callback std::function callback for SubsystemStatusResp
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const SubsystemStatusResp* const)> callback) = 0;

		/// @brief Callback Registration for
		/// @param callback std::function callback for DiscreteStatus
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ams::iface::mel::DiscreteStatus* const)> callback) = 0;

		/// @brief Callback Registration for MFA_SecurityAuditRecord
		/// @param callback std::function callback for MFA_SecurityAuditRecord
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementation
		virtual Return registerMetadataCallback(
			std::function<void(Channel& channel, const ams::iface::mel::MFA_SecurityAuditRecord* const)> callback) = 0;

		/// @brief Callback Registration for MFA_StatusDetailed
		/// @param callback std::function callback for MFA_StatusDetailed
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///     callback is already registered for this datatype on this channel
		/// @Required This function supports callback registration and must be provided by the implementer for all IR MEL implementation
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ams::iface::mel::MFA_StatusDetailed* const)> callback) = 0;

		/// @brief Callback Registration for NUC_TempData
		/// @param callback std::function callback for NUC_TempData
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///    callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration and IR MEL implementation is optional.
		/// Intended for use by IR MFAs that perform NUC calibration
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const NUC_TempData* const)> callback) = 0;

		using Channel::registerMetadataCallback;
		using Channel::send;

		HealthStatusChannel() = default;
		HealthStatusChannel(const HealthStatusChannel&) = delete;
		HealthStatusChannel& operator=(HealthStatusChannel&) = delete;
		HealthStatusChannel(HealthStatusChannel&& other) = delete;
		HealthStatusChannel& operator=(HealthStatusChannel&& other) = delete;
	};
	// end interface HealthStatusChannel

} // end namespace ams::iface::irmel
