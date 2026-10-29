//===============================================================================
/// @file  Control.h
/// @brief This file includes the Control class, used to configure channels and send commands.

#pragma once

#include <irmel/library/irmel-types/Buffer.h>
#include <irmel/library/irmel-types/Channel.h>
#include <irmel/library/irmel-types/Config.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	class Channel;
	class Config;

	/// @class Control
	/// @brief Sends a command to control the channel of an image stream.
	/// @Required This class provides required IR MEL functionality for reporting control, and must be provided by the implementer in
	/// all IR MEL implementations. See individual members for details.
	class Control
	{
	public:
		/// @Required This function supports channel control and must be provided by the implementer for all IR MEL implementations
		virtual ~Control() = default;
		/**
		 * @brief Initialize IR MEL library instance
		 */
		/// @Required This function supports IR MEL library instance and must be provided by the implementer for all IR MEL implementations
		virtual Return init(const std::string& apertureConfigID = "") = 0;
		/**
		 * @brief Get the capabilities of the MFA
		 *
		 * @return A vector of ChannelCapability types
		 */
		/// @Required This function supports ChannelCapability types and be must be provided by the implementer for all IR MEL implementations
		[[nodiscard]] virtual const std::vector<ChannelCapability>& getCapabilities() const = 0;
		/// @brief Gets the version information of the MFA
		/// @Required This function supports version information and must be provided by the implementer for all IR MEL implementations
		[[nodiscard]] virtual ams::iface::mel::VersionInfo getVersionInfo() const = 0;
		/// @brief Attaches a channel to the MFA
		/// @Required This function supports attaching channels and must be provided by the implementer for all IR MEL implementations.
		/// See the ChannelType enum for additional information on which channels must be supported
		virtual std::shared_ptr<Channel> attachChannel(const Config& config) = 0;
		/// @brief Detaches a channel from the MFA
		/// @Required This function supports detaching channels and must be provided by the implementer for all IR MEL implementations
		virtual Return detachChannel(std::shared_ptr<Channel> channel) = 0;

		Control() = default;
		Control(const Control&) = delete;
		Control& operator=(Control&) = delete;
		Control(Control&& other) = delete;
		Control& operator=(Control&& other) = delete;
	};

// extern "C" of a function that returns a shared_ptr is not supported by clang as it
//    notes that if C called this function, it would have trouble converting a shared_ptr
//    to C as there is no direct translation. This warning is being ignored as this function
//    is not being called by C, but requires C-Linkage for dlopen and dlsym calls as a result
//    of the dynamic library loading in LibraryLoader.cpp
#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
	extern "C" IR_MEL_EXPORT std::shared_ptr<Control> getControl(std::string_view instance, std::shared_ptr<::API_Manager> sam);
#if __clang__
#pragma clang diagnostic pop
#endif
} // end namespace ams::iface::irmel
