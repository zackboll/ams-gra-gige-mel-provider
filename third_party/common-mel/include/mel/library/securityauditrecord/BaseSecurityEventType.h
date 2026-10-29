#pragma once
#include <chrono>
#include <variant>
#include <vector>
#include <string>
#include "../UCI_ID.h"

namespace ams::iface::mel
{
	/// @class BaseSecurityEventType
	/// @brief A base type for the security event types to build from
	/// @Optional This class defines data that can be used in publishing Security Audit Records and is an optional component
	class BaseSecurityEventType
	{
	public:
		BaseSecurityEventType() = default;
		explicit BaseSecurityEventType(const std::string& details_in) : details{details_in}
		{
		}
		virtual ~BaseSecurityEventType() = default;
		BaseSecurityEventType(const BaseSecurityEventType&) = default;
		BaseSecurityEventType(BaseSecurityEventType&&) = default;
		BaseSecurityEventType& operator=(const BaseSecurityEventType&) = default;
		BaseSecurityEventType& operator=(BaseSecurityEventType&&) = default;

		const std::string& getDetails() const
		{
			return details;
		}
		void setDetails(const std::string& newValue)
		{
			details = newValue;
		}

	private:
		/// Specifies the category of the event as account access
		std::string details;
	};
} // end namespace ams::iface::mel
