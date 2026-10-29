//===============================================================================
/// @file  ModeCmd.h
/// @brief This file includes the data definition of the Mode Command.

#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/ScanParam.h>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class ModeCmd
	/// @brief Used to change the state and/or Mode of the MFA. See MFA_State.
	/// @Required This class provides data definition in support of IR MEL functionality to report mode commands
	/// and must be included as-is in all IR MEL implementations
	class ModeCmd
	{
	public:
		ModeCmd() = default;
		~ModeCmd() = default;
		ModeCmd(const ModeCmd&) = default;
		ModeCmd(ModeCmd&&) = default;
		ModeCmd& operator=(const ModeCmd&) = default;
		ModeCmd& operator=(ModeCmd&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] ams::iface::mel::MFA_State getState() const
		{
			return this->state;
		}
		void setState(ams::iface::mel::MFA_State state_in)
		{
			this->state = state_in;
		}
		[[nodiscard]] const MFA_Mode& getMode() const
		{
			return this->mode;
		}
		void setMode(const MFA_Mode& mode_in)
		{
			this->mode = mode_in;
		}
		[[nodiscard]] const ScanParam& getScanParameters() const
		{
			return this->scanParameters;
		}
		void setScanParameters(const ScanParam& scanParameters_in)
		{
			this->scanParameters = scanParameters_in;
		}

	private:
		std::uint32_t commandID{0};
		ams::iface::mel::MFA_State state{ams::iface::mel::MFA_State::NotSet};
		MFA_Mode mode{MFA_Mode::Unused};
		ScanParam scanParameters; ///< ScanVolumeSched mode only
	};
} // end namespace ams::iface::irmel
