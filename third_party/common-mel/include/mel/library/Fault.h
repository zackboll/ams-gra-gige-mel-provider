#pragma once
#include <chrono>
#include <cstdint>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

#include "UCI_ID.h"

namespace ams::iface::mel
{
	/// @enum FaultSeverity
	/// @brief TBD
	/// @Required This enumeration class defines data used in MEL function definitions to indicate fault severities
	/// and must be included as-is in all MEL implementations.
	enum class FaultSeverity : std::uint32_t
	{
		NotSet, /// < enum has not been set
		/// fault does not impact the current mission but may indicate something that should
		/// be checked in maintenance.  Faults should be tagged as nominal if they are targeting
		/// maintenance.  An example might be air filter hasn't been changed in 5000 hours or queue
		/// length reached 60% of max.
		Nominal,
		/// indicates things that could main later into warnings or failures.  Faults should be
		/// tagged as caution if the system is still able to comply within its performance characteristics,
		/// but may not be at its optimal.  Faults should be tagged with caution if the user should be made
		/// aware of them, but they do not affect the component state.
		Caution,
		/// A fault that carries a warning level indicates a definite problem with the subsystem has
		/// occurred but the component/capability associated with the fault can still be used.  Faults
		/// should be tagged with warning if component state associated with the fault needs to be marked
		/// degraded (note a component can still be failed if another fault with a failed label is reported).
		/// Capabilities associated with the warning, may no longer meet the performance requirements the
		/// subsystem is expected to provide or may become unavailable should the warning condition not clear.
		Warning,
		/// A fault that carries a failed level indicates the failure of a component which may preclude
		/// the use of a capability. Faults should be tagged with failed if the component state will be marked faulted.
		Failed,
		MaxExclusive ///< maximun enum item

	};

	/// @enum FaultState
	/// @brief TBD
	/// @Required This enumeration class defines data used in MEL function definitions to indicate fault states
	/// and must be included as-is in all MEL implementations.
	enum class FaultState : std::uint32_t
	{
		NotSet,		 /// < enum has not been set
		Set,		 ///< Fault has occurred
		Cleared,	 ///< Fault has been cleared
		Unknown,	 ///< Fault is in an unknown state
		MaxExclusive ///< maximun enum item
	};

	/// @class FaultData
	/// @brief Reports the data that caused the fault.  Contains key/value pair along with format
	/// and unit indicated for the data value. This type is aligned with the OMS/ UCI FaultDataType.
	/// @Required This class provides data definition in support of required MEL functionality to report faults
	/// and must be included as-is in all MEL implementations.
	class FaultData
	{
	public:
		FaultData() = default;
		FaultData(std::string k, std::string v, std::string f, std::string u)
			: key{std::move(k)}, value{std::move(v)}, format{std::move(f)}, units{std::move(u)}
		{
		}
		~FaultData() = default;
		FaultData(const FaultData&) = default;
		FaultData(FaultData&&) = default;
		FaultData& operator=(const FaultData&) = default;
		FaultData& operator=(FaultData&&) = default;

		[[nodiscard]] const std::string& getKey() const
		{
			return this->key;
		}

		void setKey(const std::string& newValue)
		{
			this->key = newValue;
		}

		[[nodiscard]] const std::string& getValue() const
		{
			return this->value;
		}

		void setValue(const std::string& newValue)
		{
			this->value = newValue;
		}

		[[nodiscard]] const std::string& getFormat() const
		{
			return this->format;
		}

		void setFormat(const std::string& newValue)
		{
			this->format = newValue;
		}

		[[nodiscard]] const std::string& getUnits() const
		{
			return this->units;
		}

		void setUnits(const std::string& newValue)
		{
			this->units = newValue;
		}

	private:
		std::string key;	///< This is the amplifying data for the fault indicating what the value represents.
		std::string value;	///< This is the amplifying data for the fault indicating what the value is.
		std::string format; ///< The format of the value e.g. string, double, int, ...
		std::string units;	///< The unit for the value e.g. Deg F, Hz, ... This field is optional, it can be empty
	};

	/// @class FaultAmbiguityGroup
	/// @brief Identifies a group of components that a Subsystem has identified as being
	/// potentially related to a fault.  Also identifies diagnostic tests (when known) that may be
	/// run to further isolate the fault. This type is aligned with the OMS/ UCI SubsystemFaultAmbiguityGroupType
	/// @Required This class provides data definition in support of required MEL functionality to report fault ambiguities
	/// and must be included as-is in all MEL implementations.
	class FaultAmbiguityGroup
	{
	public:
		FaultAmbiguityGroup() = default;
		FaultAmbiguityGroup(std::vector<UCI_ID> diag, std::vector<UCI_ID> comp) : diagnosticTestID{std::move(diag)}, componentID{std::move(comp)}
		{
		}
		~FaultAmbiguityGroup() = default;
		FaultAmbiguityGroup(const FaultAmbiguityGroup&) = default;
		FaultAmbiguityGroup(FaultAmbiguityGroup&&) = default;
		FaultAmbiguityGroup& operator=(const FaultAmbiguityGroup&) = default;
		FaultAmbiguityGroup& operator=(FaultAmbiguityGroup&&) = default;

		[[nodiscard]] const std::vector<UCI_ID>& getDiagnosticTestID() const
		{
			return this->diagnosticTestID;
		}

		// replace the existing vector with a new vector
		void setDiagnosticTestID(const std::vector<UCI_ID>& newValue)
		{
			this->diagnosticTestID = newValue;
		}

		// add a new element to the vector
		void addDiagnosticTestID(const UCI_ID& id)
		{
			this->diagnosticTestID.push_back(id);
		}

		[[nodiscard]] const std::vector<UCI_ID>& getComponentID() const
		{
			return this->componentID;
		}

		// replace the existing vector with a new vector
		void setComponentID(const std::vector<UCI_ID>& newValue)
		{
			this->componentID = newValue;
		}

		// add a new element to the vector
		void addComponentID(const UCI_ID& id)
		{
			this->componentID.push_back(id);
		}

	private:
		/// Identifies tests that may be used to further isolate the fault among the identified Components.
		/// There is no direct relationship implied between a test in this list and a ComponentID in the sibling list.
		std::vector<UCI_ID> diagnosticTestID;
		std::vector<UCI_ID> componentID; ///< Indicates the unique IDs of Components that may be related to the fault.
	};

	/// @class Fault
	/// @brief Contains fault data for reporting by subsystems and services. This type is aligned
	/// with the OMS/ UCI SubsystemFaultType.
	/// @Required This class provides data definition in support of required MEL functionality to report faults
	/// and must be included as-is in all MEL implementations.
	class Fault
	{
	public:
		Fault() = default;
		Fault(UCI_ID id, FaultSeverity sev, FaultState st, std::vector<FaultData> fdata, std::chrono::nanoseconds dect, std::string fcode,
			  std::string fdesc, std::vector<UCI_ID> comps, std::vector<FaultAmbiguityGroup> group)
			: faultID{std::move(id)},
			  severity{sev},
			  state{st},
			  faultData{std::move(fdata)},
			  detectionTime{dect},
			  faultCode{std::move(fcode)},
			  faultDescription{std::move(fdesc)},
			  componentID{std::move(comps)},
			  ambiguityGroup{std::move(group)}
		{
		}
		~Fault() = default;
		Fault(const Fault&) = default;
		Fault(Fault&&) = default;
		Fault& operator=(const Fault&) = default;
		Fault& operator=(Fault&&) = default;

		[[nodiscard]] const UCI_ID& getFaultID() const
		{
			return this->faultID;
		}

		void setFaultID(const UCI_ID& newValue)
		{
			this->faultID = newValue;
		}

		[[nodiscard]] const FaultSeverity& getSeverity() const
		{
			return this->severity;
		}

		void setSeverity(FaultSeverity newValue)
		{
			this->severity = newValue;
		}

		[[nodiscard]] const FaultState& getState() const
		{
			return this->state;
		}

		void setState(FaultState newValue)
		{
			this->state = newValue;
		}

		[[nodiscard]] const std::vector<FaultData>& getFaultData() const
		{
			return this->faultData;
		}

		// replace the existing vector with a new vector
		void setFaultData(const std::vector<FaultData>& newValue)
		{
			this->faultData = newValue;
		}

		// add a new element to the vector
		void addFaultData(const FaultData& fd)
		{
			this->faultData.push_back(fd);
		}

		[[nodiscard]] std::chrono::nanoseconds getDetectionTime() const
		{
			return this->detectionTime;
		}

		void setDetectionTime(std::chrono::nanoseconds newValue)
		{
			this->detectionTime = newValue;
		}

		[[nodiscard]] const std::string& getFaultCode() const
		{
			return this->faultCode;
		}

		void setFaultCode(const std::string& newValue)
		{
			this->faultCode = newValue;
		}

		[[nodiscard]] const std::string& getFaultDescription() const
		{
			return this->faultDescription;
		}

		void setFaultDescription(const std::string& newValue)
		{
			this->faultDescription = newValue;
		}

		[[nodiscard]] const std::vector<UCI_ID>& getComponentID() const
		{
			return this->componentID;
		}

		void setComponentID(const std::vector<UCI_ID>& newValue)
		{
			this->componentID = newValue;
		}

		// add a new element to the vector
		void addComponentID(const UCI_ID& id)
		{
			this->componentID.push_back(id);
		}

		[[nodiscard]] const std::vector<FaultAmbiguityGroup>& getAmbiguityGroup() const
		{
			return this->ambiguityGroup;
		}

		// replace the existing vector with a new vector
		void setAmbiguityGroup(const std::vector<FaultAmbiguityGroup>& newValue)
		{
			this->ambiguityGroup = newValue;
		}

		// add a new element to the vector
		void addFaultAmbiguityGroup(const FaultAmbiguityGroup& grp)
		{
			this->ambiguityGroup.push_back(grp);
		}

	private:
		/// The unique ID for this fault.  Every fault from a Subsystem, Capability, SupportCapability,
		/// Component, or Service should have a unique ID.
		UCI_ID faultID;
		FaultSeverity severity{FaultSeverity::NotSet};
		FaultState state{FaultState::NotSet};
		/// Additional fault data which contains key/value pairs with format and unit indicated
		/// for the data value. This data supports fault isolation and helps to minimize false alarms.
		std::vector<FaultData> faultData;
		/// Indicates the time the fault was detected; it does not indicate the time the fault was
		/// transmitted on the ASB, in nanoseconds UTC
		std::chrono::nanoseconds detectionTime{0};
		std::string faultCode; ///< Indicates the "code" or name of the fault.
		/// Indicates a human readable description of the fault and/or its cause. This could be
		/// different than DescriptiveLabel in the sibling FaultID field.  For example, DescriptiveLabel
		/// could be "Engine 1 overheat" and the FaultDescription could be "Lubrication oil" or "Exhaust
		/// nozzle" indicating the underlying cause of the fault.
		std::string faultDescription;
		/// Indicates the unique ID of a Component related to the fault.  When multiple components are
		/// identified, it should be inferred that each Component has been individually determined to be
		/// related to the fault.  In situations where the Subsystem has not yet been able to isolate the
		/// fault to individual components, the sibling AmbiguityGroup element should be used.
		std::vector<UCI_ID> componentID;
		/// Indicates that a fault has been narrowed down to a set of components, but not yet fully isolated.
		/// Separate AmbiguityGroups can be provided when it is desirable to associate specific tests with a
		/// subset of the set of components.
		std::vector<FaultAmbiguityGroup> ambiguityGroup;
	};
} // end namespace ams::iface::mel
