#pragma once
#include <chrono>
#include <variant>
#include <vector>
#include <string>
#include "BaseSecurityEventType.h"

namespace ams::iface::mel
{
	/// @class SecurityIntegrityType
	/// @brief Security Event for Integrity
	/// @Optional This class defines data that can be used in publishing Security Audit Records and is an optional component
	class SecurityIntegrityType : public BaseSecurityEventType
	{
	public:
		/// @enum SecurityIntegrityEnum
		/// @brief An enumeration specifying the type of integrity check performed
		/// @Optional This enumeration class defined data can be used in publishing Security Audit Records and is an optional component
		enum class SecurityIntegrityEnum
		{
			NotSet,
			ConfigFileIntegrityCheck, /// < Specifies the integrity check as a Configuration File integrity check
			BootloaderIntegrityCheck, /// < Specifies the integrity check as a Bootloader integrity check
			OFP_IntegrityCheck,		  /// < Specifies the integrity check as an OFP File integrity check
			MDF_IntegrityCheck,		  /// < Specifies the integrity check as an MDF File integrity check
			OtherFileIntegrityCheck,  /// < Specifies the integrity check as an Other File integrity check
			OtherIntegrityCheck,	  /// < Specifies the integrity check as an Other integrity check
			MaxExclusive
		};

		SecurityIntegrityType() = default;
		SecurityIntegrityType(const SecurityIntegrityEnum& category_in, const UCI_ID& MDF_ID_in, const std::string& details_in)
			: BaseSecurityEventType(details_in), category{category_in}, MDF_ID{MDF_ID_in}
		{
		}
		~SecurityIntegrityType() override = default;
		SecurityIntegrityType(const SecurityIntegrityType&) = default;
		SecurityIntegrityType(SecurityIntegrityType&&) = default;
		SecurityIntegrityType& operator=(const SecurityIntegrityType&) = default;
		SecurityIntegrityType& operator=(SecurityIntegrityType&&) = default;

		const UCI_ID& getMDF_ID() const
		{
			return MDF_ID;
		}
		void setMDF_ID(const UCI_ID& newValue)
		{
			MDF_ID = newValue;
		}

		const SecurityIntegrityEnum& getCategory() const
		{
			return category;
		}
		void setCategory(const SecurityIntegrityEnum& newValue)
		{
			category = newValue;
		}

	private:
		/// Specifies the category of the event as account access
		SecurityIntegrityEnum category{SecurityIntegrityEnum::NotSet};
		/// Used for extra details if IntegrityEventType is MDF_INTEGRITY_CHECK
		UCI_ID MDF_ID;
	};
} // end namespace ams::iface::mel
