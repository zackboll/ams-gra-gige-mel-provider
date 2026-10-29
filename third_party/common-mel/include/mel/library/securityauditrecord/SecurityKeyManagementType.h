#pragma once
#include <chrono>
#include <variant>
#include <vector>
#include <string>
#include "BaseSecurityEventType.h"
#include "SecurityFileManagementType.h"

namespace ams::iface::mel
{
	/// @class SecurityKeyManagementType
	/// @brief Key Management Event for key and certificate management operations such as key load, generation, update,
	/// deletion, or zeroization
	/// @Optional This class defines data that can be used in publishing Security Audit Records and is an optional component
	class SecurityKeyManagementType : public BaseSecurityEventType
	{
	public:
		/// @enum SecurityKeyManagementEnum
		/// @brief Specifies the category of the event as key management operations
		/// @Optional This enumeration class defined data can be used in publishing Security Audit Records and is an optional component
		enum class SecurityKeyManagementEnum
		{
			NotSet,
			KeyLoad,			 /// < Specifies the key management operation as a key has been loaded
			KeyGeneration,		 /// < Specifies the key management operation as a key has been generated
			KeyUpdate,			 /// < Specifies the key management operation as a key has been updated
			KeyDeletion,		 /// < Specifies the key management operation as a key has been deleted
			CertificateLoad,	 /// < Specifies the key management operation as a certificate has been loaded
			CertificateDeletion, /// < Specifies the key management operation as a certificate has been deleted
			MaxExclusive
		};

		SecurityKeyManagementType() = default;
		SecurityKeyManagementType(const std::string& details_in, const SecurityKeyManagementEnum& category_in)
			: BaseSecurityEventType(details_in), category{category_in}
		{
		}
		~SecurityKeyManagementType() override = default;
		SecurityKeyManagementType(const SecurityKeyManagementType&) = default;
		SecurityKeyManagementType(SecurityKeyManagementType&&) = default;
		SecurityKeyManagementType& operator=(const SecurityKeyManagementType&) = default;
		SecurityKeyManagementType& operator=(SecurityKeyManagementType&&) = default;

		const SecurityKeyManagementEnum& getCategory() const
		{
			return category;
		}
		void setCategory(const SecurityKeyManagementEnum& newValue)
		{
			category = newValue;
		}

	private:
		/// Specifies the category of the event as key management operations
		SecurityKeyManagementEnum category{SecurityKeyManagementEnum::NotSet};
	};
} // end namespace ams::iface::mel
