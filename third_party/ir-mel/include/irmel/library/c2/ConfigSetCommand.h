//===============================================================================
/// @file  ConfigSetCommand.h
/// @brief This file includes the data definition of the Config Set Command.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class ConfigSetCommand
	/// @brief Reports commands given to the configuration set
	/// @Required This class provides data definition insupoort of required IR MEL functionality to report config commands
	/// and must be included as-is in all IR MEL implementations
	class ConfigSetCommand
	{
	public:
		ConfigSetCommand() = default;
		~ConfigSetCommand() = default;
		ConfigSetCommand(const ConfigSetCommand&) = default;
		ConfigSetCommand(ConfigSetCommand&&) = default;
		ConfigSetCommand& operator=(const ConfigSetCommand&) = default;
		ConfigSetCommand& operator=(ConfigSetCommand&&) = default;

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
		[[nodiscard]] const std::string& getConfig() const
		{
			return this->config;
		}
		void setConfig(const std::string& config_in)
		{
			this->config = config_in;
		}

	private:
		std::uint32_t commandID{0};				///< Unique command id
		std::chrono::nanoseconds systemTime{0}; ///< System time for this command, in nanoseconds
		std::string config; ///< In the future maybe this becomes a defined structure part of the MEL, but for now make it a generic string
	};
} // end namespace ams::iface::irmel
