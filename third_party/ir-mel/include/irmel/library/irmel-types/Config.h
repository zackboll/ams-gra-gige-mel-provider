//===============================================================================
/// @file  Config.h
/// @brief This file includes the configuration data definition required to acquire a channel.

#pragma once

#include <memory>
#include <utility>

#include <irmel/library/irmel-types/ChannelType.h>
#include <mel/library/MFA_Component.h>
#include <mel/library/UCI_ID.h>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	class ImageListener;

	/// MFP to MFA
	/// @class Config
	/// @brief Reports the configuration of image listener data
	/// @Required This class provides data definition in support of IR MEL functionality to report image configure
	/// and must be included as-is in all IR MEL implementations
	class Config
	{
	public:
		Config() = default;
		Config(ams::iface::mel::UCI_ID cID, ChannelType ct, ams::iface::mel::UCI_ID plat, ams::iface::mel::ComponentLocation sl,
			   std::shared_ptr<ImageListener> listen, bool supSystemTrackData, bool supTrackDataUpdates)
			: chanID{std::move(cID)},
			  channelType{ct},
			  platform{std::move(plat)},
			  sensorLocation{std::move(sl)},
			  imgLstnr{std::move(listen)},
			  supportsSystemTrackData{supSystemTrackData},
			  supportsTrackDataUpdates{supTrackDataUpdates}
		{
		}

		~Config() = default;
		Config(const Config&) = default;
		Config(Config&&) = default;
		Config& operator=(const Config&) = default;
		Config& operator=(Config&&) = default;

		[[nodiscard]] const ams::iface::mel::UCI_ID& getChanID() const
		{
			return this->chanID;
		}
		void setChanID(ams::iface::mel::UCI_ID chanID_in)
		{
			this->chanID = chanID_in;
		}
		[[nodiscard]] const ChannelType& getChannelType() const
		{
			return this->channelType;
		}
		void setChannelType(ChannelType channelType_in)
		{
			this->channelType = channelType_in;
		}
		[[nodiscard]] const ams::iface::mel::UCI_ID& getPlatform() const
		{
			return this->platform;
		}
		void setPlatform(ams::iface::mel::UCI_ID platform_in)
		{
			this->platform = platform_in;
		}
		[[nodiscard]] const ams::iface::mel::ComponentLocation& getSensorLocation() const
		{
			return this->sensorLocation;
		}
		void setSensorLocation(ams::iface::mel::ComponentLocation sensorLocation_in)
		{
			this->sensorLocation = sensorLocation_in;
		}
		[[nodiscard]] const std::shared_ptr<ImageListener>& getImgLstnr() const
		{
			return this->imgLstnr;
		}
		void setImgLstnr(std::shared_ptr<ImageListener> imgLstnr_in)
		{
			this->imgLstnr = imgLstnr_in;
		}
		[[nodiscard]] bool getSupportsSystemTrackData() const
		{
			return this->supportsSystemTrackData;
		}
		void setSupportsSystemTrackData(bool supportsSystemTrackData_in)
		{
			this->supportsSystemTrackData = supportsSystemTrackData_in;
		}
		[[nodiscard]] bool getSupportsTrackDataUpdates() const
		{
			return this->supportsTrackDataUpdates;
		}
		void setSupportsTrackDataUpdates(bool supportsTrackDataUpdates_in)
		{
			this->supportsTrackDataUpdates = supportsTrackDataUpdates_in;
		}

	private:
		ams::iface::mel::UCI_ID chanID;
		ChannelType channelType{ChannelType::IRSTTrack};
		ams::iface::mel::UCI_ID platform;
		ams::iface::mel::ComponentLocation sensorLocation;
		std::shared_ptr<ImageListener> imgLstnr{};
		bool supportsSystemTrackData{false}; ///< true indicates that the sender (Skill/Service) can provide track data through RequestSystemTrackData
											 ///< and SystemTrackDataResponse
		bool supportsTrackDataUpdates{false}; ///< true indicates that the sender (Skill/Service) can provide track data through SystemTrackDataUpdate
	};
} // end namespace ams::iface::irmel
