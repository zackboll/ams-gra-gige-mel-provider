//===============================================================================
/// @file  ChannelCommsTestRep.h
/// @brief This file includes the response to a Channel Comms Test.

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class ChannelCommsTestRep
	/// @brief This struct is the response to ChannelCommsTestReq
	/// The response is sent on the channel indicated by channelID in the request
	/// @Required This class provides data defintion in support of required IR MEL functionality to report communication channel tests
	/// and must be included as-is in all IR MEL implementations
	class ChannelCommsTestRep
	{
	public:
		ChannelCommsTestRep() = default;
		explicit ChannelCommsTestRep(std::uint32_t cmdId, std::uint32_t id) : commandID{cmdId}, requestID{id}
		{
		}
		~ChannelCommsTestRep() = default;
		ChannelCommsTestRep(const ChannelCommsTestRep&) = default;
		ChannelCommsTestRep(ChannelCommsTestRep&&) = default;
		ChannelCommsTestRep& operator=(const ChannelCommsTestRep&) = default;
		ChannelCommsTestRep& operator=(ChannelCommsTestRep&&) = default;

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
		std::uint32_t requestID{0};
	};
} // end namespace ams::iface::irmel
