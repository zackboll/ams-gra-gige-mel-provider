//===============================================================================
/// @file  StackFrameHeader.h
/// @brief This file includes StackFrameHeader

#pragma once

#include <utility>
#include <cstdint>
#include <string>
#include <mel/library/UCI_ID.h>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
/// Of interest is the Command Type enum class,
/// which documents all commands that can be sent from the MFP to the MFA.
/// The Classes in the file
/// describe how the connections between the MFA and MFP are made.
namespace ams::iface::irmel
{
	/// @class StackFrameHeader
	/// @brief Provides the defining attributes and format of a TSI Image Stack
	/// @RequiredIfImageStack This class provides data definition in support of TSI Image Stacks
	class StackFrameHeader
	{
	public:
		StackFrameHeader() = default;
		StackFrameHeader(std::uint16_t platID, std::uint8_t fom, std::uint16_t stackD, ams::iface::mel::UCI_ID capId, ams::iface::mel::UCI_ID actId,
						 std::uint32_t stackId_in, std::uint32_t packPixSize, std::uint16_t height_in, std::uint16_t width_in, std::uint8_t bitD,
						 std::uint8_t pixelD, std::uint32_t mask)
			: platformId{platID},
			  txaFOM{fom},
			  stackDepth{stackD},
			  capabilityId{std::move(capId)},
			  activityId{std::move(actId)},
			  stackId{stackId_in},
			  packedPixelsSize{packPixSize},
			  height{height_in},
			  width{width_in},
			  bitDepth{bitD},
			  pixelDepth{pixelD},
			  deadPixelMask{mask}
		{
		}
		~StackFrameHeader() = default;
		StackFrameHeader(const StackFrameHeader&) = default;
		StackFrameHeader(StackFrameHeader&&) = default;
		StackFrameHeader& operator=(const StackFrameHeader&) = default;
		StackFrameHeader& operator=(StackFrameHeader&&) = default;

		[[nodiscard]] std::uint16_t getplatformId() const
		{
			return platformId;
		}
		void setplatformId(std::uint16_t id)
		{
			this->platformId = id;
		}
		[[nodiscard]] std::uint8_t getTxaFOM() const
		{
			return txaFOM;
		}
		void setTxaFOM(std::uint8_t t)
		{
			this->txaFOM = t;
		}
		[[nodiscard]] std::uint16_t getStackDepth() const
		{
			return stackDepth;
		}
		void setStackDepth(std::uint16_t depth)
		{
			this->stackDepth = depth;
		}
		[[nodiscard]] ams::iface::mel::UCI_ID getCapabilityId() const
		{
			return capabilityId;
		}
		void setCapabilityId(ams::iface::mel::UCI_ID id)
		{
			this->capabilityId = id;
		}
		[[nodiscard]] ams::iface::mel::UCI_ID getActivityId() const
		{
			return activityId;
		}
		void setActivityId(ams::iface::mel::UCI_ID id)
		{
			this->activityId = id;
		}
		[[nodiscard]] std::uint32_t getStackId() const
		{
			return stackId;
		}
		void setStackId(std::uint32_t id)
		{
			this->stackId = id;
		}
		[[nodiscard]] std::uint32_t getPackedPixelsSize() const
		{
			return packedPixelsSize;
		}
		void setPackedPixelsSize(std::uint32_t packedPixelsSize_in)
		{
			this->packedPixelsSize = packedPixelsSize_in;
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

		[[nodiscard]] std::uint8_t getBitDepth() const
		{
			return bitDepth;
		}
		void setBitDepth(std::uint8_t depth)
		{
			this->bitDepth = depth;
		}
		[[nodiscard]] std::uint8_t getPixelDepth() const
		{
			return pixelDepth;
		}
		void setPixelDepth(std::uint8_t depth)
		{
			this->pixelDepth = depth;
		}
		[[nodiscard]] std::uint32_t getDeadPixelMask() const
		{
			return deadPixelMask;
		}
		void setDeadPixelMask(std::uint32_t mask)
		{
			this->deadPixelMask = mask;
		}

	private:
		std::uint16_t platformId{0};		  ///< ID of the platform
		std::uint8_t txaFOM{0};				  ///< TXA figure of merit (1-10; 1 = best performance)
		std::uint16_t stackDepth{0};		  ///< How many frames are stacked in the pixel data
		ams::iface::mel::UCI_ID capabilityId; ///< Used to uniquely identify the stack in conjunction with activityId and stackId
		ams::iface::mel::UCI_ID activityId;	  ///< Used to uniquely identify the stack in conjunction with capabilityId and stackId
		std::uint32_t stackId{0};			  ///< Used to uniquely identify the stack in conjunction with capabilityId and activityId
		std::uint32_t packedPixelsSize{0};	  ///< The total size of all packed pixels data for this stack (bitDepth X height X width X stackDepth)
		std::uint16_t height{0};			  ///< Frame height in pixels
		std::uint16_t width{0};				  ///< Frame width in pixels
		std::uint8_t bitDepth{0};			  ///< The number of bits in each pixel data field, multiple of 8
		std::uint8_t pixelDepth{0};			  ///< The number of logical bits of data from the camera in each pixel data field
		std::uint32_t deadPixelMask{
			0}; ///< The mask that, when applied to the pixel data field, contains Dead Pixel information for that pixel data field
	};
} // end namespace ams::iface::irmel
