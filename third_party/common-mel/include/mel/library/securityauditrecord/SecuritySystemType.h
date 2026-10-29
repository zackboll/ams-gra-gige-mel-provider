#pragma once
#include <chrono>
#include <variant>
#include <vector>
#include <string>
#include "BaseSecurityEventType.h"

namespace ams::iface::mel
{
	/// @class SecuritySystemType
	/// @brief system Event for Startup, reset, shutdown, fault and other events specific to the system
	/// @Optional This class defines data that can be used in publishing Security Audit Records and is an optional component
	class SecuritySystemType : public BaseSecurityEventType
	{
	public:
		/// @enum SecuritySystemEnum
		/// @brief Specifies the category of the event as system events
		/// @Optional This enumeration class defined data can be used in publishing Security Audit Records and is an optional component
		enum class SecuritySystemEnum
		{
			NotSet,
			Startup,  /// < Specifies the system state event as a system startup
			Shutdown, /// < Specifies the system state event as a system shutdown
			Reset,	  /// < Specifies the system state event as a system reset
			Fault,	  /// < Specifies the system state event as a system fault
			MaxExclusive
		};

		SecuritySystemType() = default;
		SecuritySystemType(const std::string& details_in, const SecuritySystemEnum& category_in) : BaseSecurityEventType{details_in}, category{category_in}
		{
		}
		~SecuritySystemType() override = default;
		SecuritySystemType(const SecuritySystemType&) = default;
		SecuritySystemType(SecuritySystemType&&) = default;
		SecuritySystemType& operator=(const SecuritySystemType&) = default;
		SecuritySystemType& operator=(SecuritySystemType&&) = default;

		const SecuritySystemEnum& getCategory() const
		{
			return category;
		}
		void setCategory(const SecuritySystemEnum& newValue)
		{
			category = newValue;
		}

	private:
		/// Specifies the category of the event as system events
		SecuritySystemEnum category{SecuritySystemEnum::NotSet};
	};
} // end namespace ams::iface::mel
