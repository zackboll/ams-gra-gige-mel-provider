#pragma once
#include <chrono>
#include <variant>
#include <vector>
#include <string>
#include "BaseSecurityEventType.h"

namespace ams::iface::mel
{
	/// @class SecuritySanitizationType
	/// @brief Specifies the type of audit event as sanitization
	/// @Optional This class defines data that can be used in publishing Security Audit Records and is an optional component
	class SecuritySanitizationType : public BaseSecurityEventType
	{
	public:
		/// @enum SecuritySanitizationEnum
		/// @brief Specifies the type of audit event as sanitization
		/// @Optional This enumeration class defined data can be used in publishing Security Audit Records and is an optional component
		enum class SecuritySanitizationEnum
		{
			NotSet,
			Zeroization,		 /// < Specifies the sanitization operation as a zeroization
			Sanitization,		 /// < Specifies the sanitization operation as a sanitization
			ClassifiedDataErase, /// < Specifies the sanitization operation as a classified data erase
			MaxExclusive
		};

		SecuritySanitizationType() = default;
		SecuritySanitizationType(const std::string& details_in, const SecuritySanitizationEnum& category_in)
			: BaseSecurityEventType(details_in), category{category_in}
		{
		}
		~SecuritySanitizationType() override = default;
		SecuritySanitizationType(const SecuritySanitizationType&) = default;
		SecuritySanitizationType(SecuritySanitizationType&&) = default;
		SecuritySanitizationType& operator=(const SecuritySanitizationType&) = default;
		SecuritySanitizationType& operator=(SecuritySanitizationType&&) = default;

		const SecuritySanitizationEnum& getCategory() const
		{
			return category;
		}
		void setCategory(const SecuritySanitizationEnum& newValue)
		{
			category = newValue;
		}

	private:
		/// Specifies the category of the event as system events
		SecuritySanitizationEnum category{SecuritySanitizationEnum::NotSet};
	};
} // end namespace ams::iface::mel
