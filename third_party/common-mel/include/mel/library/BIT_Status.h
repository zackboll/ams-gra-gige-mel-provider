#pragma once
#include <string>
#include <vector>
#include <utility>

#include "ActiveBIT.h"
#include "CompletedBIT.h"
#include "Fault.h"

namespace ams::iface::mel
{
	/// @class BIT_Status
	/// @brief This type is aligned with the OMS/ UCI SubsystemBIT_StatusMDT.
	/// @Required This class provides data definition in support of required MEL functionality to report the status of BITs
	/// and must be included as-is in all MEL implementations.
	class BIT_Status
	{
	public:
		BIT_Status() = default;
		BIT_Status(std::vector<ActiveBIT> act, std::vector<CompletedBIT> comp, std::vector<Fault> fau)
			: activeBITs{std::move(act)}, completedBITs{std::move(comp)}, faults{std::move(fau)}
		{
		}
		~BIT_Status() = default;
		BIT_Status(const BIT_Status&) = default;
		BIT_Status(BIT_Status&&) = default;
		BIT_Status& operator=(const BIT_Status&) = default;
		BIT_Status& operator=(BIT_Status&&) = default;

		[[nodiscard]] const std::vector<ActiveBIT>& getActiveBITs() const
		{
			return this->activeBITs;
		}

		// replace the existing vector with a new vector
		void setActiveBITs(const std::vector<ActiveBIT>& newValue)
		{
			this->activeBITs = newValue;
		}

		// add a new element to the vector
		void addActiveBIT(const ActiveBIT& bit)
		{
			this->activeBITs.push_back(bit);
		}

		[[nodiscard]] const std::vector<CompletedBIT>& getCompletedBITs() const
		{
			return this->completedBITs;
		}

		// replace the existing vector with a new vector
		void setCompletedBITs(const std::vector<CompletedBIT>& newValue)
		{
			this->completedBITs = newValue;
		}

		// add a new element to the vector
		void addCompletedBIT(const CompletedBIT& bit)
		{
			this->completedBITs.push_back(bit);
		}

		[[nodiscard]] const std::vector<Fault>& getFaults() const
		{
			return this->faults;
		}

		// replace the existing vector with a new vector
		void setFaults(const std::vector<Fault>& newValue)
		{
			this->faults = newValue;
		}

		// add a new element to the vector
		void addFault(const Fault& fault)
		{
			this->faults.push_back(fault);
		}

	private:
		/// Indicates a BIT that is currently running or enqueued to run.  This element can be used
		/// for BIT initiated via SubsystemStateCommand, SubsystemBIT_Command and/or by the Subsystem
		/// itself (periodic BIT (PBIT), background BIT (BBIT), start-up BIT (SBIT), etc.).
		std::vector<ActiveBIT> activeBITs;
		/// Indicates results of a previously completed BIT.  This element can be used for results
		/// from BIT initiated via SubsystemStateCommand, SubsystemBIT_Command and/or self-initiated
		/// by the Subsystem.  Subsystem self-initiated BIT examples include periodic BIT (PBIT),
		/// background BIT (BBIT), startup BIT (SBIT), maintenance BIT (MBIT), etc.
		std::vector<CompletedBIT> completedBITs;
		/// Indicates a current fault in the Subsystem.  When omitted, the Subsystem has no
		/// current faults.  Faults aren't necessarily related to or exclusively detected by BIT.
		/// This element is independent of the sibling BIT_Results so that faults can be reported
		/// regardless of how they were detected or triggered.
		std::vector<Fault> faults;
	};
} // end namespace ams::iface::mel
