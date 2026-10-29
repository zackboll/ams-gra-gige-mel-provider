//===============================================================================
/// @file  Channel.h
/// @brief This file includes the base Channel type
//===============================================================================
#pragma once

#include <irmel/library/irmel-types/ChannelCapability.h>
#include <irmel/library/irmel-types/ChannelCommsTestRep.h>
#include <irmel/library/irmel-types/ChannelCommsTestReq.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/Control.h>
#include <mel/library/CalibrationStatus.h>
#include <mel/library/CommonMEL.h>
#include <mel/library/DiscreteStatus.h>
#include <mel/library/SubsystemCalibration.h>
#include <mel/library/securityauditrecord/MFA_SecurityAuditRecord.h>
#include <functional>
#include <memory>

namespace ams::iface::irmel
{
	/// @class Channel
	/// @brief Reports the data that as channel's image stram receives.
	/// @Required This class provides required IR MEL functionality for reporting channel details, and must be provided by the implementer
	/// in all IR MEL implementations. See individual members for details
	class Channel
	{
	public:
		/// @Required This function supports channel and must be provided by the implementer for all IR MEL implementations
		virtual ~Channel() = default;

		/// @brief Returns  after sending sendKeepAliveRep
		/// @Required This function is used to inform MFA that a Service is connected when there are no other commands and must be provided by the
		/// implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<Return> sendKeepAliveRep() = 0;

		/// @brief Returns ChannelCommsTestRep after sending ChannelCommsTestReq
		/// @param[in] channelCommsTestReq - Command to send
		/// @Required This function allow for the specific command to be sent and must be provided by the implementer for all IR MEL implementations
		virtual ams::iface::mel::RequestFor<ChannelCommsTestRep> send(ChannelCommsTestReq channelCommsTestReq) = 0;

		/**
		 * @brief Registers a buffer to receive frame data on the channel's image stream. Buffers can only be called prior to
		 * Channel_obj::enable() or after Channel_obj::disable().
		 *
		 * @param buffer The buffer to register with the channel to use for frame data.
		 * @return Error_E::MEL_BAD_POINTER is any of the pointers are invalid
		 */
		/// @Required This function supports registered buffer frame data and must be provided by the implementer for all IR MEL implementations
		virtual Return registerBuffer(std::shared_ptr<Buffer> buffer) = 0;
		/// @brief Unregisters a buffer to receive frame data on the channel's image stream.
		/// @Required This function supports unregistered buffer frame data and must be provided by the implementer for all IR MEL implementations
		virtual Return unregisterBuffer(std::shared_ptr<Buffer> buffer) = 0;
		/// @brief Enables frame data displayed on channel's image stream
		/// @Required This function supports enable frame data and must be provided by the implementer for all IR MEL implementations
		virtual Return enable() = 0;
		/// @brief Disables frame data of channel's image stream
		/// @Required This function supports diabled frame data and must be provided by the implementer for all IR MEL implementations
		virtual Return disable() = 0;
		/// @brief Get the capabilities of the channel's image stream
		/// @Required This function supports passing channel capabilities and must be provided by the implementer  for all IR MEL implementations
		[[nodiscard]] virtual ChannelCapability getCapabilities() const = 0;

		/// @brief Callback Registration for ChannelCommsTestRep
		/// @param[in] callback - std::function callback for ChannelCommsTestRep
		/// @return Return::Success if first time called, Return::Fail otherwise
		/// @Required This function supports performing channel comms tests and must be provided by the implementer for all IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const ChannelCommsTestRep* const)> callback) = 0;

		Channel() = default;
		Channel(const Channel&) = delete;
		Channel& operator=(Channel&) = delete;
		Channel(Channel&& other) = delete;
		Channel& operator=(Channel&& other) = delete;
	};
	// end interface Channel

} // end namespace ams::iface::irmel
