//===============================================================================
/// @file  CameraCommandResp.h
/// @brief This file includes the Camera Command Resonse.

#pragma once

#include <chrono>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class CameraCommandResp
	/// @brief MFA reports whether the parameter updates based on the CameraCommand were successful
	/// @RequiredIfCameraCtrl This class provides data definition in support of Camera Control
	/// and must be included as-is in all IR MEL implementations for IR MFAs that support programmable control of camera
	class CameraCommandResp
	{
	public:
		CameraCommandResp() = default;
		~CameraCommandResp() = default;
		CameraCommandResp(const CameraCommandResp&) = default;
		CameraCommandResp(CameraCommandResp&&) = default;
		CameraCommandResp& operator=(const CameraCommandResp&) = default;
		CameraCommandResp& operator=(CameraCommandResp&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::uint32_t getSize() const
		{
			return this->size;
		}
		void setSize(std::uint32_t size_in)
		{
			this->size = size_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getTimestamp() const
		{
			return this->timestamp;
		}
		void setTimestamp(std::chrono::nanoseconds timestamp_in)
		{
			this->timestamp = timestamp_in;
		}
		[[nodiscard]] bool getCommandSuccess() const
		{
			return this->commandSuccess;
		}
		void setCommandSuccess(bool commandSuccess_in)
		{
			this->commandSuccess = commandSuccess_in;
		}

	private:
		std::uint32_t commandID{0};
		std::uint32_t size{0};
		std::chrono::nanoseconds timestamp{0};
		bool commandSuccess{false}; ///< true indicates that the MFA is able to satisfy all requested parameters
	};
} // namespace ams::iface::irmel
