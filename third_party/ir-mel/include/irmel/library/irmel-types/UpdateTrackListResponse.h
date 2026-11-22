//===============================================================================
/// @file  UpdateTrackListResponse.h
/// @brief This file includes the UpdateTrackListResponse class definition.

#pragma once

#include <irmel/library/irmel-types/TrackListResponse.h>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class UpdateTrackListResponse
	/// @brief When the service uses sendCommand to send PrioritizedTrackList CommandType, a response from the MFA indicating whether or not the
	/// MFA will schedule a track is sent back to the Service via the UpdateTrackListResponseCallback.
	/// @RequiredIfScanVolumeSchedulingOnMFA This class provides data definition in support of required IR MEL functionality to report updated list
	/// responses and must be included as-is in all IR MEL implementations for MFAs that support ScanVolumeSchedule
	class UpdateTrackListResponse
	{
	public:
		UpdateTrackListResponse() = default;
		~UpdateTrackListResponse() = default;
		UpdateTrackListResponse(const UpdateTrackListResponse&) = default;
		UpdateTrackListResponse(UpdateTrackListResponse&&) = default;
		UpdateTrackListResponse& operator=(const UpdateTrackListResponse&) = default;
		UpdateTrackListResponse& operator=(UpdateTrackListResponse&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] const TrackListResponse& getTrackListResponse() const
		{
			return this->trackListResponse;
		}
		void setTrackListResponse(const TrackListResponse& trackListResponse_in)
		{
			this->trackListResponse = trackListResponse_in;
		}

	private:
		std::uint32_t commandID{0};
		TrackListResponse trackListResponse{TrackListResponse::None};
	};
} // namespace ams::iface::irmel
