#pragma once
#include <chrono>
#include <variant>
#include <vector>
#include <string>
#include "BaseSecurityEventType.h"

namespace ams::iface::mel
{
	/// @class SecurityFileManagementType
	/// @brief File Management Event for file operations such as access, deletions, or modifications
	/// @Optional This class defines data that can be used in publishing Security Audit Records and is an optional component
	class SecurityFileManagementType : public BaseSecurityEventType
	{
	public:
		/// @enum SecurityFileManagementEnum
		/// @brief An enumeration specifying the type of file operation performed
		/// @Optional This class defines data that can be used in publishing Security Audit Records and is an optional component
		enum class SecurityFileManagementEnum
		{
			NotSet,
			ConfigFileUpdate, /// < Specifies the file operation as a Configuration File update
			OFP_Update,		  /// < Specifies the file operation as an OFP File update
			MDF_Update,		  /// < Specifies the file operation as an MDF File update
			OtherFileUpdate,  /// < Specifies the file operation as an Other File update
			MaxExclusive
		};

		SecurityFileManagementType() = default;
		SecurityFileManagementType(const std::string& details_in, const SecurityFileManagementEnum& category_in, const UCI_ID& MDF_ID_in)
			: BaseSecurityEventType(details_in), category{category_in}, MDF_ID{MDF_ID_in}
		{
		}
		~SecurityFileManagementType() override = default;
		SecurityFileManagementType(const SecurityFileManagementType&) = default;
		SecurityFileManagementType(SecurityFileManagementType&&) = default;
		SecurityFileManagementType& operator=(const SecurityFileManagementType&) = default;
		SecurityFileManagementType& operator=(SecurityFileManagementType&&) = default;

		const UCI_ID& getMDF_ID() const
		{
			return MDF_ID;
		}
		void setMDF_ID(const UCI_ID& newValue)
		{
			MDF_ID = newValue;
		}

		const SecurityFileManagementEnum& getCategory() const
		{
			return category;
		}
		void setCategory(const SecurityFileManagementEnum& newValue)
		{
			category = newValue;
		}

	private:
		/// Specifies the category of the event as account access
		SecurityFileManagementEnum category{SecurityFileManagementEnum::NotSet};
		/// Used for extra details if IntegrityEventType is MDF_INTEGRITY_CHECK
		UCI_ID MDF_ID;
	};
} // end namespace ams::iface::mel
