//===============================================================================
/// @file  FrameHeader.h
/// @brief This file includes the data definition of a frame header, included with all single
/// 	images.

#pragma once

#include <irmel/library/irmel-types/ChannelCapability.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/SensorInertialState.h>
#include <irmel/library/irmel-types/SensorNavState.h>
#include <cstdint>
#include <chrono>
#include <utility>
#include <vector>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class FrameHeader
	/// @brief Provides the defining attributes and format of an image frame
	/// @Required This class provides data definition associated with producing frame data,
	/// which is required functionality and must be included as-is in all IR MEL implementations
	class FrameHeader
	{
	public:
		FrameHeader() = default;
		FrameHeader(std::chrono::nanoseconds stime, std::chrono::nanoseconds itime, std::uint32_t w, std::uint32_t h, std::uint32_t bpp,
					std::uint32_t bands, double hfov, double vfov, ContributingSensor f, PixelFormat pf, std::uint32_t fId, std::uint32_t sId,
					std::uint32_t stotal, ImageType itype, ImageFlip iflip, std::vector<ImageFlag> flgs, double drow, double dcol, std::uint32_t rOff,
					std::uint32_t cOff, std::vector<SensorInertialState> state, std::vector<SensorNavState> nav, ChannelCapability::BandIndex idx)
			: systemTime{stime},
			  integrationTime{itime},
			  width{w},
			  height{h},
			  bitsPerPixel{bpp},
			  numBands{bands},
			  horizontalFieldOfView{hfov},
			  verticalFieldOfView{vfov},
			  face{std::move(f)},
			  format{pf},
			  frameID{fId},
			  subframeID{sId},
			  subframeTotal{stotal},
			  imageType{itype},
			  imageFlip{iflip},
			  flags{std::move(flgs)},
			  ditherRow{drow},
			  ditherCol{dcol},
			  rowOffset{rOff},
			  columnOffset{cOff},
			  sensorInertialState{std::move(state)},
			  sensorNavState{std::move(nav)},
			  bandIndex{idx}
		{
		}
		~FrameHeader() = default;
		FrameHeader(const FrameHeader&) = default;
		FrameHeader(FrameHeader&&) = default;
		FrameHeader& operator=(const FrameHeader&) = default;
		FrameHeader& operator=(FrameHeader&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds stime)
		{
			this->systemTime = stime;
		}
		[[nodiscard]] std::chrono::nanoseconds getIntegrationTime() const
		{
			return this->integrationTime;
		}
		void setIntegrationTime(std::chrono::nanoseconds itime)
		{
			this->integrationTime = itime;
		}
		[[nodiscard]] std::uint32_t getWidth() const
		{
			return this->width;
		}
		void setWidth(std::uint32_t w)
		{
			this->width = w;
		}
		[[nodiscard]] std::uint32_t getHeight() const
		{
			return this->height;
		}
		void setHeight(std::uint32_t h)
		{
			this->height = h;
		}
		[[nodiscard]] std::uint32_t getBitsPerPixel() const
		{
			return this->bitsPerPixel;
		}
		void setBitsPerPixel(std::uint32_t row)
		{
			this->bitsPerPixel = row;
		}
		[[nodiscard]] std::uint32_t getNumBands() const
		{
			return this->numBands;
		}
		void setNumBands(std::uint32_t num)
		{
			this->numBands = num;
		}
		[[nodiscard]] double getHorizontalFieldOfView() const
		{
			return this->horizontalFieldOfView;
		}
		void setHorizontalFieldOfView(double hfv)
		{
			this->horizontalFieldOfView = hfv;
		}
		[[nodiscard]] double getVerticalFieldOfView() const
		{
			return this->verticalFieldOfView;
		}
		void setVerticalFieldOfView(double vfv)
		{
			this->verticalFieldOfView = vfv;
		}
		[[nodiscard]] const ContributingSensor& getFace() const
		{
			return this->face;
		}
		void setFace(ContributingSensor f)
		{
			this->face = f;
		}
		[[nodiscard]] const PixelFormat& getFormat() const
		{
			return this->format;
		}
		void setFormat(PixelFormat fmat)
		{
			this->format = fmat;
		}
		[[nodiscard]] std::uint32_t getFrameID() const
		{
			return this->frameID;
		}
		void setFrameID(std::uint32_t frame)
		{
			this->frameID = frame;
		}
		[[nodiscard]] std::uint32_t getSubframeID() const
		{
			return this->subframeID;
		}
		void setSubframeID(std::uint32_t subframe)
		{
			this->subframeID = subframe;
		}
		[[nodiscard]] std::uint32_t getSubframeTotal() const
		{
			return this->subframeTotal;
		}
		void setSubframeTotal(std::uint32_t total)
		{
			this->subframeTotal = total;
		}
		[[nodiscard]] const ImageType& getImageType() const
		{
			return this->imageType;
		}
		void setImageType(ImageType image)
		{
			this->imageType = image;
		}
		[[nodiscard]] const ImageFlip& getImageFlip() const
		{
			return this->imageFlip;
		}
		void setImageFlip(ImageFlip image)
		{
			this->imageFlip = image;
		}
		[[nodiscard]] const std::vector<ImageFlag>& getFlags() const
		{
			return this->flags;
		}
		// replace the existing vector with a new vector
		void setFlags(const std::vector<ImageFlag>& flg)
		{
			this->flags = flg;
		}
		// add a new element to the vector
		void addImageFlag(const ImageFlag& flg)
		{
			this->flags.push_back(flg);
		}
		[[nodiscard]] double getDitherRow() const
		{
			return this->ditherRow;
		}
		void setDitherRow(double row)
		{
			this->ditherRow = row;
		}
		[[nodiscard]] double getDitherCol() const
		{
			return this->ditherCol;
		}
		void setDitherCol(double col)
		{
			this->ditherCol = col;
		}
		[[nodiscard]] std::uint32_t getRowOffset() const
		{
			return this->rowOffset;
		}
		void setRowOffset(std::uint32_t offset)
		{
			this->rowOffset = offset;
		}
		[[nodiscard]] std::uint32_t getColumnOffset() const
		{
			return this->columnOffset;
		}
		void setColumnOffset(std::uint32_t offset)
		{
			this->columnOffset = offset;
		}
		[[nodiscard]] const std::vector<SensorInertialState>& getSensorInertialState() const
		{
			return this->sensorInertialState;
		}
		// replace the existing vector with a new vector
		void setSensorInertialState(const std::vector<SensorInertialState>& sstate)
		{
			this->sensorInertialState = sstate;
		}
		// add a new element to the vector
		void addSensorInertialState(const SensorInertialState& sstate)
		{
			this->sensorInertialState.push_back(sstate);
		}
		[[nodiscard]] const std::vector<SensorNavState>& getSensorNavState() const
		{
			return this->sensorNavState;
		}
		// replace the existing vector with a new vector
		void setSensorNavState(const std::vector<SensorNavState>& sstate)
		{
			this->sensorNavState = sstate;
		}
		// add a new element to the vector
		void addSensorNavState(const SensorNavState& sstate)
		{
			this->sensorNavState.push_back(sstate);
		}
		[[nodiscard]] ChannelCapability::BandIndex getBandIndex() const
		{
			return this->bandIndex;
		}
		void setBandIndex(ChannelCapability::BandIndex idx)
		{
			this->bandIndex = idx;
		}

	private:
		/// System time associated with the image, in nanoseconds. For staring images,
		/// it is at the start of integration of the image. For scanning (TDI) images,
		/// it is the time associated with the first line of the image buffer.
		/// Image buffer is configured in raster, which is row major format.
		std::chrono::nanoseconds systemTime{0};
		std::chrono::nanoseconds integrationTime{0};
		std::uint32_t width{0};						 ///< Width of the image in pixels
		std::uint32_t height{0};					 ///< Height of the image in pixels
		std::uint32_t bitsPerPixel{0};				 ///< Number of bits in each pixel
		/// (bitsPerPixel * Columns) must be divisible by 8 as the row data is byte-aligned
		std::uint32_t numBands{0};				 ///< Indicates which spectral band is the image
		double horizontalFieldOfView{0};		 ///< Horizontal view of MFA in radians
		double verticalFieldOfView{0};			 ///< Vertical view of MFA in radians
		ContributingSensor face;				 ///< Indicates the sensor that provided the current image
		PixelFormat format{PixelFormat::Mono};	 ///< Indicates the format of the image
		std::uint32_t frameID{0};				 ///< Indicates the individual frame ID
		std::uint32_t subframeID{0};			 ///< Indicates the subframe ID
		std::uint32_t subframeTotal{0};			 ///< total number of subframes in 1 frame
		ImageType imageType{ImageType::Staring}; ///< Indicates the type of image
		ImageFlip imageFlip{ImageFlip::None};	 ///< Indicates image flipped configuration
		std::vector<ImageFlag> flags{};			 ///< List of flags associated with the image
		/// For microscanned images, indicates the dither offset of the image in the row direction.
		/// Positive values indicate an image that has been dithered such that an object that was at
		/// row Y now appears at Y minus dither_row.
		double ditherRow{0};
		/// For microscanned images, indicates the dither offset of the image in the column direction.
		/// Positive values indicate an image that has been dithered such that an object that was at column
		/// X now appears at X minus dither_col.
		double ditherCol{0};
		/// These next two could be changed to type double and replace the ditherRow/ditherCol
		/// and support the same functionality of either dither for microscanned or offset for subframe.
		std::uint32_t rowOffset{0};	   ///< Offset number of rows for the start of the frame in sensor coordinates
		std::uint32_t columnOffset{0}; ///< Offset in columns for the start of the frame in sensor coordinates
		/// Vector of sensor inertial data per column during image corrolating
		/// For a non-TDI frame, sensorInertialState results in a 1 element vector.
		/// For a TDI frame, each element of sensorInertialState corresponds to each a column.
		std::vector<SensorInertialState> sensorInertialState{};
		/// A vector of representations of the sensor's internal navigational state at the time this frame
		/// was recorded. The frame of reference being used in each element is indicated by an enum within the element.
		std::vector<SensorNavState> sensorNavState{};
		ChannelCapability::BandIndex bandIndex{
			0}; ///< uint8_t that Indicates band(s) used to generate image. See imageBands map on ChannelCapability.
	};
} // end namespace ams::iface::irmel
