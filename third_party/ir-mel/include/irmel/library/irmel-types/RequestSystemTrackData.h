//===============================================================================
/// @file  RequestSystemTrackData.h
/// @brief This file includes the RequestSystemTrackData.

#pragma once

#include <chrono>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class RequestSystemTrackData
	/// @brief This struct is used for the MFA to request track information from the MFP through onMetadata
	/// @Optional This class provides data definition in support of IR MEL functionality to report track information
	/// or enhance MFA OEM software related to pointing. While optional for IR MEL implementations, inclusion in
	/// a particular MFA instantiation indicates the importance of track data being provided to the MFA.  Failure to
	/// match support of this feature in a corresponding Service likely jeopardizes the ability of the MFA to provide
	/// high quality tracks and may have follow-on effects for pointing accuracy.
	class RequestSystemTrackData
	{
	public:
		RequestSystemTrackData() = default;
		~RequestSystemTrackData() = default;
		RequestSystemTrackData(const RequestSystemTrackData&) = default;
		RequestSystemTrackData(RequestSystemTrackData&&) = default;
		RequestSystemTrackData& operator=(const RequestSystemTrackData&) = default;
		RequestSystemTrackData& operator=(RequestSystemTrackData&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
		}
		[[nodiscard]] std::uint32_t getRequestId() const
		{
			return this->requestId;
		}
		void setRequestId(std::uint32_t requestId_in)
		{
			this->requestId = requestId_in;
		}
		[[nodiscard]] std::uint32_t getTrackId() const
		{
			return this->trackId;
		}
		void setTrackId(std::uint32_t trackId_in)
		{
			this->trackId = trackId_in;
		}

		std::chrono::nanoseconds systemTime{0};
		std::uint32_t commandID{0};
		std::uint32_t requestId{0};
		std::uint32_t trackId{0};
	};
} // namespace ams::iface::irmel
