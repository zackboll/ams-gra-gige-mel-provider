#pragma once
#include <chrono>
#include <variant>
#include <vector>
#include "BaseSecurityEventType.h"
#include "SecurityArtifact.h"
#include "SecurityAuthenticationType.h"
#include "SecurityFileManagementType.h"
#include "SecurityIntegrityType.h"
#include "SecurityKeyManagementType.h"
#include "SecuritySanitizationType.h"
#include "SecuritySystemType.h"

namespace ams::iface::mel
{
	/// @class MFA_SecurityAuditRecord
	/// @brief Contains Security Audit Record information. See individual field annotations for more information. MFAs are only expected to report
	/// event types relevant to their type and design. MFAs are not required to produce all event types expressed in this enumeration. This type is
	/// aligned with the OMS/UCI SecurityAuditRecord.
	/// @Required This enumeration class defined data used in publishing Security Audit Records and must be included as-is in all MEL implementations
	class MFA_SecurityAuditRecord
	{
	public:
		/// @enum OutcomeEnum
		/// @brief An enumeration specifying the outcome of the operation
		/// @Optional This enumeration class defined data can be used in publishing Security Audit Records and is an optional component
		enum class OutcomeEnum
		{
			NotSet,
			Failure, /// < Specifies that the event or operation failed
			Success, /// < Specifies that the event or operation succeeded
			MaxExclusive
		};

		/// @enum SeverityEnum
		/// @brief An enumeration specifying the significance of the audit event
		/// @Optional This enumeration class defined data can be used in publishing Security Audit Records and is an optional component
		enum class SeverityEnum
		{
			NotSet,
			Critical, /// < A security event has been detected and significant information or mechanisms have been compromised. The system may no
					  /// longer be assumed to be operating correctly in support of the security policy. Immediate action is required.
			Error, /// < A security event has been detected and less significant information or mechanisms have been compromised. Action is required
				   /// within a given time.
			Informational, /// < Normal operational messages that may be collected for reporting, measuring throughput, etc. No action is required.
			Warning,	   /// < A warning indication that a security event may occur if action is not taken.
			MaxExclusive
		};

		/// @brief A NameSpace specifying the type of security event
		using SecurityEventType = std::variant<std::monostate, SecurityAuthenticationType, SecurityIntegrityType, SecurityFileManagementType,
											   SecurityKeyManagementType, SecuritySystemType, SecuritySanitizationType>;

		MFA_SecurityAuditRecord() = default;
		MFA_SecurityAuditRecord(const UCI_ID& securityEventID_in, std::chrono::nanoseconds& eventTimestamp_in, const UCI_ID& subsystemID_in,
								std::vector<SecurityArtifact>& securityArtifacts_in, SecurityEventType eventType_in, OutcomeEnum outcome_in,
								SeverityEnum severity_in)
			: securityEventID{securityEventID_in},
			  eventTimestamp{eventTimestamp_in},
			  subsystemID{subsystemID_in},
			  securityArtifacts{securityArtifacts_in},
			  eventType{eventType_in},
			  outcome{outcome_in},
			  severity{severity_in}
		{
		}
		~MFA_SecurityAuditRecord() = default;
		MFA_SecurityAuditRecord(const MFA_SecurityAuditRecord&) = default;
		MFA_SecurityAuditRecord(MFA_SecurityAuditRecord&&) = default;
		MFA_SecurityAuditRecord& operator=(const MFA_SecurityAuditRecord&) = default;
		MFA_SecurityAuditRecord& operator=(MFA_SecurityAuditRecord&&) = default;

		[[nodiscard]] const UCI_ID& getSecurityEventID() const
		{
			return securityEventID;
		}
		void setSecurityEventID(const UCI_ID& newValue)
		{
			this->securityEventID = newValue;
		}
		[[nodiscard]] const std::chrono::nanoseconds& getEventTimestamp() const
		{
			return eventTimestamp;
		}
		void setEventTimestamp(const std::chrono::nanoseconds& newValue)
		{
			this->eventTimestamp = newValue;
		}
		[[nodiscard]] const UCI_ID& getSubsystemID() const
		{
			return subsystemID;
		}
		void setSubsystemID(const UCI_ID& newValue)
		{
			this->subsystemID = newValue;
		}
		[[nodiscard]] const std::vector<SecurityArtifact>& getSecurityArtifacts() const
		{
			return securityArtifacts;
		}
		void setSecurityArtifacts(const std::vector<SecurityArtifact>& newValue)
		{
			this->securityArtifacts = newValue;
		}
		[[nodiscard]] const SecurityEventType& getEventType() const
		{
			return eventType;
		}
		void setEventType(const SecurityEventType& newValue)
		{
			this->eventType = newValue;
		}
		[[nodiscard]] const OutcomeEnum& getOutcome() const
		{
			return outcome;
		}
		void setOutcome(const OutcomeEnum& newValue)
		{
			this->outcome = newValue;
		}
		[[nodiscard]] const SeverityEnum& getSeverity() const
		{
			return severity;
		}
		void setSeverity(const SeverityEnum& newValue)
		{
			this->severity = newValue;
		}

	private:
		/// Indicates a unique ID assigned to the Security Event
		UCI_ID securityEventID;
		/// Indicates timestamp for when the Security Event occured
		std::chrono::nanoseconds eventTimestamp{0};
		/// Indicates the unique ID assigned to the subsystem reporting the event
		UCI_ID subsystemID;
		/// List of artifacts from Security Event
		std::vector<SecurityArtifact> securityArtifacts;
		/// Specific information regarding the audited Security Relevant Events
		SecurityEventType eventType;
		/// The outcome of the Security Event (success or failure)
		OutcomeEnum outcome{OutcomeEnum::NotSet};
		/// Severity of the Security Event being reported
		SeverityEnum severity{SeverityEnum::NotSet};
	};
} // end namespace ams::iface::mel
