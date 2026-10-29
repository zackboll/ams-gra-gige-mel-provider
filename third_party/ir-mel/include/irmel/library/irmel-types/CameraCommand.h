//===============================================================================
/// @file  CameraCommand.h
/// @brief This file includes the data definition of the Camera Command.

#pragma once

#include <cstdint>

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <chrono>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class CameraCommand
	/// @brief Indicates the commands given to the camera
	/// @RequiredIfCameraCtrl This class provides data definition in support of Camera Control
	/// and must be included as-is in all IR MEL implementations for IR MFAs that support programmable control of camera
	class CameraCommand
	{
	public:
		CameraCommand() = default;
		~CameraCommand() = default;
		CameraCommand(const CameraCommand&) = default;
		CameraCommand(CameraCommand&&) = default;
		CameraCommand& operator=(const CameraCommand&) = default;
		CameraCommand& operator=(CameraCommand&&) = default;

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
		[[nodiscard]] const ImageType& getImageType() const
		{
			return this->imageType;
		}
		void setImageType(const ImageType& imageType_in)
		{
			this->imageType = imageType_in;
		}
		[[nodiscard]] std::uint32_t getWidth() const
		{
			return this->width;
		}
		void setWidth(std::uint32_t width_in)
		{
			this->width = width_in;
		}
		[[nodiscard]] std::uint32_t getHeight() const
		{
			return this->height;
		}
		void setHeight(std::uint32_t height_in)
		{
			this->height = height_in;
		}
		[[nodiscard]] std::uint32_t getFrameRate() const
		{
			return this->frameRate;
		}
		void setFrameRate(std::uint32_t frameRate_in)
		{
			this->frameRate = frameRate_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getIntegrationTime() const
		{
			return this->integrationTime;
		}
		void setIntegrationTime(std::chrono::nanoseconds integrationTime_in)
		{
			this->integrationTime = integrationTime_in;
		}
		[[nodiscard]] std::uint32_t getRowOffset() const
		{
			return this->rowOffset;
		}
		void setRowOffset(std::uint32_t rowOffset_in)
		{
			this->rowOffset = rowOffset_in;
		}
		[[nodiscard]] std::uint32_t getColumnOffset() const
		{
			return this->columnOffset;
		}
		void setColumnOffset(std::uint32_t columnOffset_in)
		{
			this->columnOffset = columnOffset_in;
		}
		[[nodiscard]] const ScanDir& getDirection() const
		{
			return this->direction;
		}
		void setDirection(const ScanDir& direction_in)
		{
			this->direction = direction_in;
		}
		[[nodiscard]] std::uint32_t getNumberOfStages() const
		{
			return this->numberOfStages;
		}
		void setNumberOfStages(std::uint32_t numberOfStages_in)
		{
			this->numberOfStages = numberOfStages_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0};	 ///< System time for this command, in nanoseconds
		ImageType imageType{ImageType::Staring}; ///< Specify the framing mode for the camera
		std::uint32_t commandID{0};					 ///< Unique command id
		std::uint32_t width{0};						 ///< Requested window width for the frame, has to be within the system, in pixels
		std::uint32_t height{0};					 ///< Requested window height for the frame, has to be within the system, in pixels
		std::uint32_t frameRate{0};					 ///< in hertz
		std::chrono::nanoseconds integrationTime{0}; ///< in nanoseconds
		std::uint32_t rowOffset{0};					 ///< Offset in rows for the start of the frame in sensor coordinates
		std::uint32_t columnOffset{0};				 ///< Offset in columns for the start of the frame in sensor coordinates
		ScanDir direction{ScanDir::NegToPosAz};		 ///< When scanning, direction to scan
		std::uint32_t numberOfStages{0};			 ///< When scanning, number of TDI stages to use
	};
} // end namespace ams::iface::irmel
