//===============================================================================
/// @file  ChannelCommsTestReq.h
/// @brief This file includes the request for a Channel Comms Test.

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class ChannelCommsTestReq
	/// @brief  Used to test communication between the MFA and MFP for track channels.
	/// The request is sent through a command and Control channel, and the response is
	/// sent on the particular channel of interest, indicated by channelID.
	/// @Required This class provides data definition in support of required IR MEL functionality to send communication test request
	/// of a channel and must be included as-is in all IR MEL implementations
	class ChannelCommsTestReq
	{
	public:
		ChannelCommsTestReq() = default;
		~ChannelCommsTestReq() = default;
		ChannelCommsTestReq(const ChannelCommsTestReq&) = default;
		ChannelCommsTestReq(ChannelCommsTestReq&&) = default;
		ChannelCommsTestReq& operator=(const ChannelCommsTestReq&) = default;
		ChannelCommsTestReq& operator=(ChannelCommsTestReq&&) = default;

		[[nodiscard]] std::uint32_t getChannelID() const
		{
			return this->channelID;
		}
		void setChannelID(std::uint32_t channelID_in)
		{
			this->channelID = channelID_in;
		}
		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::uint32_t getRequestID() const
		{
			return this->requestID;
		}
		void setRequestID(std::uint32_t requestID_in)
		{
			this->requestID = requestID_in;
		}

	private:
		std::uint32_t commandID{0};
		std::uint32_t channelID{0};
		std::uint32_t requestID{0};
	};
} // end namespace ams::iface::irmel
