#pragma once
#include <chrono>
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

#include "UCI_ID.h"

namespace ams::iface::mel
{
	/// @enum BIT_ControlInterface
	/// @brief Indicates the supported BIT initialization type for a subsystem
	/// @Required This enumeration class defines data used in MEL function definitions to indicate subystem states
	/// as it pertains to bit configuration and must be included as-is in all MEL implementations.
	enum class BIT_ControlInterface : std::uint32_t
	{
		NotSet, /// < enum has not been set
		/// Indicates the Subsystem supports initiated/manual BIT via the SubsystemBIT_Command message.
		/// Support of initiated BIT via this message implies the Subsystem can perform the BIT without
		/// changing the Subsystem state.  For example, if the Subsystem was in an OPERATE state,
		/// it can perform initiated/manual BIT interleaved with operational functions and capability.
		SubsystemBITCommand,
		/// Indicates the Subsystem supports self-initiated BIT.  For example, startup BIT (SBIT),
		/// periodic BIT (PBIT), etc.
		SubsystemStateCommand,
		/// Indicates the Subsystem supports initiated/manual BIT via the SubsystemStateCommand
		/// message.  Support of initiated BIT via this message implies the Subsystem will change
		/// the overall Subsystem state to INITIATED_BIT while BIT is in in progress.
		/// For example, if the Subsystem was in an OPERATE state then commanded to perform BIT via
		/// the SubsystemStateCommand message, it will interrupt operational functions and capability
		/// while BIT is in progress.
		SubsystemInitiated,
		MaxExclusive ///< maximun enum item
	};

	/// @class BIT_Type
	/// @brief Indicates a BIT supported by the Subsystem. This type is
	/// aligned with the OMS/ UCI SubsystemBIT_Type.
	/// @note The CapabiltyID field resident in the corresponding OMS/ UCI
	/// type is not present here because MFAs notionally do not provide OMS Capabilities.
	/// @Required This class provides data definition in support of required MEL functionality to interpret
	/// BIT commands from various OMS Message sources and must be included as-is in all MEL implementations.
	class BIT_Type
	{
	public:
		BIT_Type() = default;
		BIT_Type(UCI_ID id, BIT_ControlInterface control, std::vector<std::string> name, std::vector<UCI_ID> sub, std::chrono::nanoseconds dur)
			: bitID{std::move(id)},
			  acceptedInterface{control},
			  bitItemName{std::move(name)},
			  subsystemComponentID{std::move(sub)},
			  expectedDuration{dur}
		{
		}
		~BIT_Type() = default;
		BIT_Type(const BIT_Type&) = default;
		BIT_Type(BIT_Type&&) = default;
		BIT_Type& operator=(const BIT_Type&) = default;
		BIT_Type& operator=(BIT_Type&&) = default;

		[[nodiscard]] const UCI_ID& getBitID() const
		{
			return this->bitID;
		}

		void setBitID(const UCI_ID& newValue)
		{
			this->bitID.setUUID(newValue.getUUID());
			this->bitID.setDescriptiveLabel(newValue.getDescriptiveLabel());
		}

		[[nodiscard]] const BIT_ControlInterface& getAcceptedInterface() const
		{
			return this->acceptedInterface;
		}

		void setAcceptedInterface(BIT_ControlInterface newValue)
		{
			this->acceptedInterface = newValue;
		}

		[[nodiscard]] const std::vector<std::string>& getBitItemName() const
		{
			return this->bitItemName;
		}

		// replace the existing vector with a new vector
		void setBitItemNames(const std::vector<std::string>& newValue)
		{
			this->bitItemName = newValue;
		}

		// add a new element to the vector
		void addBitItemName(const std::string& name)
		{
			this->bitItemName.push_back(name);
		}

		[[nodiscard]] const std::vector<UCI_ID>& getSubsystemComponentID() const
		{
			return this->subsystemComponentID;
		}

		// replace the existing vector with a new vector
		void setSubsystemComponentID(const std::vector<UCI_ID>& newValue)
		{
			this->subsystemComponentID = newValue;
		}

		// add a new element to the vector
		void addSubsystemComponentID(const UCI_ID& id)
		{
			this->subsystemComponentID.push_back(id);
		}

		[[nodiscard]] std::chrono::nanoseconds getExpectedDuration() const
		{
			return this->expectedDuration;
		}

		void setExpectedDuration(std::chrono::nanoseconds newValue)
		{
			this->expectedDuration = newValue;
		}

	private:
		/// Indicates the unique ID of the BIT along with its human readable name.  This ID is used
		/// to initiate the BIT via SubsystemBIT_Command and status the BIT via SubsystemBIT_Status.
		UCI_ID bitID;
		/// Indicates the BIT messaging interface that is supported by the BIT.  See enumeration
		/// annotations for further details. Notice that only one enumeration value is supported.
		BIT_ControlInterface acceptedInterface{BIT_ControlInterface::NotSet};
		/// Indicates the name of a non-Component, non-Capability item tested by the BIT.
		/// BIT items can be logical or physical, singular or aggregated. This field is optional, it can be empty.
		std::vector<std::string> bitItemName;
		/// Indicates a Component that is at least partially tested by the BIT.
		/// This field is optional, it can be empty.
		std::vector<UCI_ID> subsystemComponentID;
		/// Allows a Subsystem to specify expected duration of the test, when known, in nanoseconds.
		/// This field is optional, it can e be empty.
		std::chrono::nanoseconds expectedDuration{0};
	};

	/// @class BIT_Configuration
	/// @brief This type is aligned with the OMS/ UCI SubsystemBIT_ConfigurationMDT.
	/// @Required This class provides data definition in support of required MEL functionality to report BIT Types
	/// and must be included as-is in all MEL implementations.
	class BIT_Configuration
	{
	public:
		BIT_Configuration() = default;
		explicit BIT_Configuration(std::vector<BIT_Type> b) : bit{std::move(b)}
		{
		}
		~BIT_Configuration() = default;
		BIT_Configuration(const BIT_Configuration&) = default;
		BIT_Configuration(BIT_Configuration&&) = default;
		BIT_Configuration& operator=(const BIT_Configuration&) = default;
		BIT_Configuration& operator=(BIT_Configuration&&) = default;

		[[nodiscard]] const std::vector<BIT_Type>& getBit() const
		{
			return this->bit;
		}

		// replace the existing vector with a new vector
		void setBit(const std::vector<BIT_Type>& newValue)
		{
			this->bit = newValue;
		}

		// add a new element to the vector
		void addBIT_Type(const BIT_Type& type)
		{
			this->bit.push_back(type);
		}

	private:
		std::vector<BIT_Type> bit;
	};
} // end namespace ams::iface::mel
