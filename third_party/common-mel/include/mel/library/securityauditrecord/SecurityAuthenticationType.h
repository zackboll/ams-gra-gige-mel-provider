#pragma once
#include <string>
#include <chrono>
#include <variant>
#include <vector>
#include "BaseSecurityEventType.h"

namespace ams::iface::mel
{
	/// @class SecurityAuthenticationType
	/// @brief Authentication Event for subsystems, services, files
	/// @Required This enumeration class defined data used in publishing Security Audit Records and must be included as-is in all MEL
	/// implementations
	class SecurityAuthenticationType : public BaseSecurityEventType
	{
	public:
		/// @enum SecurityAuthenticationEnum
		/// @brief An enumeration specifying the type of authentication performed
		/// @Required This enumeration class defined data used in publishing Security Audit Records and must be included as-is in all MEL
		/// implementations
		enum class SecurityAuthenticationEnum
		{
			NotSet,
			SubsystemAuthentication,   /// < Specifies the authentication as a Subsystem authentication
			ServiceAuthentication,	   /// < Specifies the authentication as a Service authentication
			ConfigFileAuthentication,  /// < Specifies the authentication as a Configuration File authentication
			OFP_Authentication,		   /// < Specifies the authentication as an OFP File authentication
			MDF_Authentication,		   /// < Specifies the authentication as an MDF File authentication
			OtherFileAuthentication,   /// < Specifies the authentication as an Other File authentication
			OtherDeviceAuthentication, /// < Specifies the authentication as an Other Device authentication
			MaxExclusive
		};
		SecurityAuthenticationType() = default;
		SecurityAuthenticationType(const SecurityAuthenticationEnum& category_in, const std::string& details_in)
			: BaseSecurityEventType(details_in), category{category_in}
		{
		}
		~SecurityAuthenticationType() override = default;
		SecurityAuthenticationType(const SecurityAuthenticationType&) = default;
		SecurityAuthenticationType(SecurityAuthenticationType&&) = default;
		SecurityAuthenticationType& operator=(const SecurityAuthenticationType&) = default;
		SecurityAuthenticationType& operator=(SecurityAuthenticationType&&) = default;

		SecurityAuthenticationEnum getCategory() const
		{
			return category;
		}
		void setCategory(SecurityAuthenticationEnum newValue)
		{
			category = newValue;
		}
		const UCI_ID getSubsystemID() const
		{
			return subsystemID;
		}
		void setSubsystemID(const UCI_ID& newValue)
		{
			subsystemID = newValue;
		}
		const UCI_ID getServiceID() const
		{
			return serviceID;
		}
		void setServiceID(const UCI_ID& newValue)
		{
			serviceID = newValue;
		}
		const UCI_ID getMDF_ID() const
		{
			return MDF_ID;
		}
		void setMDF_ID(const UCI_ID& newValue)
		{
			MDF_ID = newValue;
		}

	private:
		/// Specifies the category of the event as account access
		SecurityAuthenticationEnum category{SecurityAuthenticationEnum::NotSet};
		/// Used for extra details if AuthenticationEventType is SUBSYSTEM_AUTHENTICATION
		UCI_ID subsystemID;
		/// Used for extra details if AuthenticationEventType is SERVICE_AUTHENTICATION
		UCI_ID serviceID;
		/// Used for extra details if AuthenticationEventType is MDF_AUTHENTICATION
		UCI_ID MDF_ID;
	};
} // end namespace ams::iface::mel
