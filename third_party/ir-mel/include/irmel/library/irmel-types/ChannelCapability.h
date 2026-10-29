//===============================================================================
/// @file  ChannelCapability.h
/// @brief This file includes the data definition of channel capability information

#pragma once

#include <irmel/library/irmel-types/BandInfo.h>
#include <irmel/library/irmel-types/ChannelMetadataCapabilityType.h>
#include <irmel/library/irmel-types/ChannelType.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/SensorNavState.h>
#include <mel/library/CommonMEL.h>
#include <map>
#include <set>
#include <vector>
#include <cstdint>
#include <utility>

namespace ams::iface::irmel
{
	/// @class ChannelCapability
	/// @brief Used in the Channel and Control classes to report a Channel or the MFA's capabilities
	/// @Required This class provides data definition in support of IR MEL funationality to report MFA capabilities
	/// and must be included as-is in all IR MEL implementations
	class ChannelCapability
	{
	public:
		/// @brief BandIndex correlates to a Frame with a set of associated imageBands
		/// @Optional This type can be optionally used to index bandInfo from a FrameHeader
		using BandIndex = uint8_t;
		ChannelCapability() = default;
		ChannelCapability(ams::iface::mel::UCI_ID id, std::uint32_t hgt, std::uint32_t wid, std::uint32_t depth, std::uint32_t row, std::uint32_t num,
						  PixelFormat fmat, std::vector<SensorType> sens, ams::iface::mel::UCI_ID plat, ams::iface::mel::ComponentLocation loc,
						  std::vector<ChannelType> cap, std::uint32_t tsk, bool odc, bool nuc, std::set<ChannelMetadataCapabilityType> chnMetCap,
						  std::map<BandIndex, std::vector<BandInfo>> bands, std::vector<CoordinateSystemType> navFrames_in)
			: chanID{std::move(id)},
			  height{hgt},
			  width{wid},
			  bitDepth{depth},
			  rowPitch{row},
			  numberOfBands{num},
			  format{fmat},
			  sensorTypes{std::move(sens)},
			  platform{std::move(plat)},
			  sensorLocation{std::move(loc)},
			  channelTypes{std::move(cap)},
			  TaskScheduleDepth{tsk},
			  odcAvail{odc},
			  nucAvail{nuc},
			  channelMetadataCapabilities{std::move(chnMetCap)},
			  imageBands{std::move(bands)},
			  navFrames{std::move(navFrames_in)}
		{
		}
		~ChannelCapability() = default;
		ChannelCapability(const ChannelCapability&) = default;
		ChannelCapability(ChannelCapability&&) = default;
		ChannelCapability& operator=(const ChannelCapability&) = default;
		ChannelCapability& operator=(ChannelCapability&&) = default;

		[[nodiscard]] const ams::iface::mel::UCI_ID& getChanID() const
		{
			return this->chanID;
		}
		void setChanID(ams::iface::mel::UCI_ID chan)
		{
			this->chanID = chan;
		}
		[[nodiscard]] std::uint32_t getHeight() const
		{
			return this->height;
		}
		void setHeight(std::uint32_t h)
		{
			this->height = h;
		}
		[[nodiscard]] std::uint32_t getWidth() const
		{
			return this->width;
		}
		void setWidth(std::uint32_t w)
		{
			this->width = w;
		}
		[[nodiscard]] std::uint32_t getBitDepth() const
		{
			return this->bitDepth;
		}
		void setBitDepth(std::uint32_t depth)
		{
			this->bitDepth = depth;
		}
		[[nodiscard]] std::uint32_t getRowPitch() const
		{
			return this->rowPitch;
		}
		void setRowPitch(std::uint32_t row)
		{
			this->rowPitch = row;
		}
		[[nodiscard]] std::uint32_t getBufferSize() const
		{
			return this->bufferSize;
		}
		void setBufferSize(std::uint32_t bufferSize_in)
		{
			this->bufferSize = bufferSize_in;
		}
		[[nodiscard]] std::uint32_t getImageSize() const
		{
			return this->imageSize;
		}
		void setImageSize(std::uint32_t imageSize_in)
		{
			this->imageSize = imageSize_in;
		}
		[[nodiscard]] std::uint32_t getNumberOfBands() const
		{
			return this->numberOfBands;
		}
		void setNumberOfBands(std::uint32_t num)
		{
			this->numberOfBands = num;
		}
		[[nodiscard]] const PixelFormat& getFormat() const
		{
			return this->format;
		}
		void setFormat(PixelFormat f)
		{
			this->format = f;
		}
		[[nodiscard]] const std::vector<SensorType>& getSensorTypes() const
		{
			return this->sensorTypes;
		}
		/// @brief Replaces the existing vector with a new vector
		void setSensorTypes(const std::vector<SensorType>& sensorCaps)
		{
			this->sensorTypes = sensorCaps;
		}
		[[nodiscard]] const ams::iface::mel::UCI_ID& getPlatform() const
		{
			return this->platform;
		}
		void setPlatform(ams::iface::mel::UCI_ID pform)
		{
			this->platform = pform;
		}
		[[nodiscard]] const ams::iface::mel::ComponentLocation& getSensorLocation() const
		{
			return this->sensorLocation;
		}
		void setSensorLocation(ams::iface::mel::ComponentLocation location)
		{
			this->sensorLocation = location;
		}
		[[nodiscard]] const std::vector<ChannelType>& getChannelTypes() const
		{
			return this->channelTypes;
		}
		/// @brief Replaces the existing vector with a new vector
		void setChannelTypes(const std::vector<ChannelType>& channels)
		{
			this->channelTypes = channels;
		}
		[[nodiscard]] std::uint32_t getTaskScheduleDepth() const
		{
			return this->TaskScheduleDepth;
		}
		void setTaskScheduleDepth(std::uint32_t depth)
		{
			this->TaskScheduleDepth = depth;
		}
		[[nodiscard]] bool getOdcAvail() const
		{
			return this->odcAvail;
		}
		void setOdmAvail(bool odcAvail_in)
		{
			this->odcAvail = odcAvail_in;
		}
		[[nodiscard]] bool getNucAvail() const
		{
			return this->nucAvail;
		}
		void setNucAvail(bool nucAvail_in)
		{
			this->nucAvail = nucAvail_in;
		}
		/// @brief gets the full set of supported Channel Metadata types
		[[nodiscard]] const std::set<ChannelMetadataCapabilityType>& getChannelMetadataCapabilities() const
		{
			return this->channelMetadataCapabilities;
		}
		/// @brief sets (replaces) the full set of supported Channel Metadata types
		void setChannelMetadataCapabilities(const std::set<ChannelMetadataCapabilityType>& channelMetadataCapabilities_in)
		{
			this->channelMetadataCapabilities = channelMetadataCapabilities_in;
		}
		/// @brief Get the imageBands map
		[[nodiscard]] const std::map<BandIndex, std::vector<BandInfo>>& getImageBands() const
		{
			return this->imageBands;
		}
		/// @brief Replaces the existing imageBands map with a new map
		void setImageBands(const std::map<BandIndex, std::vector<BandInfo>>& bands)
		{
			this->imageBands = bands;
		}
		/// @brief Add a set of band info to the imageBands map
		void addImageBand(BandIndex bandIndex, const std::vector<BandInfo>& bands)
		{
			this->imageBands.emplace(bandIndex, bands);
		}
		/// @brief Clears the Image Bands map.
		void clearImageBands()
		{
			imageBands.clear();
		}
		/// @brief Get the vector of navigation reference frames
		[[nodiscard]] const std::vector<CoordinateSystemType>& getNavFrames() const
		{
			return this->navFrames;
		}
		/// @brief Replaces the existing navFrames vector with a new vector
		void setNavFrames(const std::vector<CoordinateSystemType>& navFrames_in)
		{
			this->navFrames = navFrames_in;
		}
		/// @brief Add a coordinate system to the navFrames vector
		void addnavFrames(const CoordinateSystemType& navFrame)
		{
			this->navFrames.push_back(navFrame);
		}

	private:
		ams::iface::mel::UCI_ID chanID;
		std::uint32_t height{0}; ///< Height of an image buffer produced by the MFA
		std::uint32_t width{0};	 ///< Width of an image buffer produced by the MFA
		/// Bit depth of the images (bit-depth of the dynamic range of the images.
		/// For example, this would be 12 for an camera that has a 12-bit ADC).
		std::uint32_t bitDepth{0};
		std::uint32_t rowPitch{0}; ///< Row pitch (in bytes) of an image buffer produced by the MFA
		std::uint32_t bufferSize{0};
		std::uint32_t imageSize{0};
		std::uint32_t numberOfBands{0}; ///< Number of different EOIR bands the MFA produces
		PixelFormat format{PixelFormat::Mono};
		/// List of capabilities for the sensor
		std::vector<SensorType> sensorTypes{SensorType::Unspecified};
		ams::iface::mel::UCI_ID platform;
		ams::iface::mel::ComponentLocation sensorLocation;
		std::vector<ChannelType> channelTypes{};
		/// Every EOIR MFA has the capability to perform Task Scheduling. TaskScheduleDepth is how many tasks the
		/// MFA can store in the queue at a time.  For a TaskScheduleDepth of n>0, the MFA can keep track of n tasks
		/// in priority order to perform after the current task is complete.  A TaskScheduleDepth of 0 means that
		/// any new task sent with IMMEDIATE priority will cause the currently executing task to be aborted and the new
		/// task to be started. This essentially means that the MFP is managing task priority and scheduling, and simply
		/// feeding the tasks to the MFA one at a time when appropriate.
		std::uint32_t TaskScheduleDepth{0};
		bool odcAvail{false}; ///< to indicate when the MFA provides an ODM for image processing support
		bool nucAvail{false}; ///< to indicate when the MFA performs NUC
		/// Indicates the set of Metadata types that are supported by a channel.
		/// Query this set to check if a specific metadata type is supported or get the full list.
		/// @note All values will be unique and bounded by the available values defined in the ChannelMetadataCapabilityType enumeration.
		/// The values in the set should correspond to the registerMetadataCallback functions of Channel class.
		/// @note Calls to a channel's registerMetadataCallback function where the channel does not have a corresponding
		/// entry in this set are expected to return Return::NotSupported.
		std::set<ChannelMetadataCapabilityType> channelMetadataCapabilities{};
		/// Provides info on the band(s) of the image. A single-band image will only declare a single map element.
		/// First element of map is a uint8_t aliased to "BandIndex". The second is a vector of BandInfo objects.
		/// Each BandIndex correlates to a Frame with a set of associated imageBands
		/// The bandIndex member of FrameHeader is used to correlate some number of bands to frames by indexing this map.
		std::map<BandIndex, std::vector<BandInfo>> imageBands{};
		/// Indicates the reference frames in which each image's navigation data will be delivered, and thus the size of the
		/// sensor nav state vector attached to each frame. Can be empty if the sensor does not provide internal navigation
		/// data. The order of types in this vector corresponds to their order in the FrameHeader.
		std::vector<CoordinateSystemType> navFrames{};
	};
} // end namespace ams::iface::irmel
