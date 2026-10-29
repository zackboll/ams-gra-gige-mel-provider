//===============================================================================
/// @file  CameraProtectCmd.h
/// @brief This file includes the data definition of the Camera Protect Command.
//===============================================================================
#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <chrono>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class CameraProtectCmd
	/// @brief Command MFA to protect camera
	/// @RequiredIfCameraProtect This class provides data definition in support of Camera Self-Protection
	/// and must be included as-is in all IR MEL implementations for IR MFAs that support camera self-protection
	class CameraProtectCmd
	{
	public:
		CameraProtectCmd() = default;
		~CameraProtectCmd() = default;
		CameraProtectCmd(const CameraProtectCmd&) = default;
		CameraProtectCmd(CameraProtectCmd&&) = default;
		CameraProtectCmd& operator=(const CameraProtectCmd&) = default;
		CameraProtectCmd& operator=(CameraProtectCmd&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getResponseTime() const
		{
			return this->responseTime;
		}
		void setResponseTime(std::chrono::nanoseconds responseTime_in)
		{
			this->responseTime = responseTime_in;
		}
		[[nodiscard]] bool getProtectFlag() const
		{
			return this->protectFlag;
		}
		void setProtectFlag(bool protectFlag_in)
		{
			this->protectFlag = protectFlag_in;
		}

	private:
		std::uint32_t commandID{0};				  ///< Unique command id
		std::chrono::nanoseconds responseTime{0}; ///< Response time for this command, in nanoseconds
		bool protectFlag{false};				  ///< Enable = True, Disable = False
	};

	/// @enum CmdResp
	/// @brief Indicates whether CameraProtectCmd was OK, Unavailable, or Unsupported
	/// @RequiredIfCameraProtect This class provides data definition in support of Camera Self-Protection
	/// and must be included as-is in all IR MEL implementations for IR MFAs that support camera self-protection
	enum class CmdResp : std::uint32_t
	{
		OK,
		Unavailable,
		Unsupported
	};

	/// @class CameraProtectCmdResp
	/// @brief MFA reports whether the parameter updates based on the CameraProtectCmd were successful
	/// @RequiredIfCameraProtect This class provides data definition in support of Camera Self-Protection
	/// and must be included as-is in all IR MEL implementations for IR MFAs that support camera self-protection
	class CameraProtectCmdResp
	{
	public:
		CameraProtectCmdResp() = default;
		~CameraProtectCmdResp() = default;
		CameraProtectCmdResp(const CameraProtectCmdResp&) = default;
		CameraProtectCmdResp(CameraProtectCmdResp&&) = default;
		CameraProtectCmdResp& operator=(const CameraProtectCmdResp&) = default;
		CameraProtectCmdResp& operator=(CameraProtectCmdResp&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getResponseTime() const
		{
			return this->responseTime;
		}
		void setResponseTime(std::chrono::nanoseconds responseTime_in)
		{
			this->responseTime = responseTime_in;
		}
		[[nodiscard]] const CmdResp& getCommandResponse() const
		{
			return this->commandResponse;
		}
		void setCommandResponse(const CmdResp& commandResponse_in)
		{
			this->commandResponse = commandResponse_in;
		}

	private:
		std::uint32_t commandID{0};				  ///< Unique command id
		std::chrono::nanoseconds responseTime{0}; ///< Response time for this command, in nanoseconds
		CmdResp commandResponse{CmdResp::OK};	  ///< indicates the MFAs capability with regards to Camera Self-Protection
	};
} // end namespace ams::iface::irmel
