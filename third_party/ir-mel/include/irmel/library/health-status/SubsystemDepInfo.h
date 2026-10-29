//===============================================================================
/// @file  SubsystemDepInfo.h
/// @brief This file includes the type of failure indicateed in a subsystem status message.

#pragma once

#include <cstdint>
#include <irmel/library/irmel-types/CommonIR_MEL.h>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class SubsystemDepInfo
	/// @brief Reports the failure status of a Subsystem
	/// @Required This class provides data definition in support of required IR MEL functionailty to report Subsystem information
	/// and must be included as-is in all IR MEL implementations
	class SubsystemDepInfo
	{
	public:
		SubsystemDepInfo() = default;
		~SubsystemDepInfo() = default;
		SubsystemDepInfo(const SubsystemDepInfo&) = default;
		SubsystemDepInfo(SubsystemDepInfo&&) = default;
		SubsystemDepInfo& operator=(const SubsystemDepInfo&) = default;
		SubsystemDepInfo& operator=(SubsystemDepInfo&&) = default;

		[[nodiscard]] std::uint32_t getSubsystemId() const
		{
			return this->subsystemId;
		}
		void setSubsystemId(std::uint32_t subsystemId_in)
		{
			this->subsystemId = subsystemId_in;
		}
		[[nodiscard]] std::uint32_t getCriticality() const
		{
			return this->criticality;
		}
		void setCriticality(std::uint32_t criticality_in)
		{
			this->criticality = criticality_in;
		}
		[[nodiscard]] Failure getFailureLevel() const
		{
			return this->failureLevel;
		}
		void setFailureLevel(Failure failureLevel_in)
		{
			this->failureLevel = failureLevel_in;
		}

	private:
		std::uint32_t subsystemId{0};
		std::uint32_t criticality{0};
		Failure failureLevel{Failure::NA};
	};
} // namespace ams::iface::irmel
