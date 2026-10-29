//===============================================================================
/// @file  SubsystemStatusResp.h
/// @brief This file includes the data defintion the MFA uses to respond with the
/// subsystem status.
//===============================================================================
#pragma once

#include <irmel/library/health-status/SubsystemCSCIInfo.h>
#include <irmel/library/health-status/SubsystemDepInfo.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <string>
#include <vector>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class SubsystemStatusResp
	/// @brief The SubsystemStatusResp serves to provide a status response from the subsystem
	/// @Required This class provides data definitions in support of IR MEL functionality to report the
	/// response status of subsystems and be included as-is in all IR MEL implementations.
	class SubsystemStatusResp
	{
	public:
		SubsystemStatusResp() = default;
		~SubsystemStatusResp() = default;
		SubsystemStatusResp(const SubsystemStatusResp&) = default;
		SubsystemStatusResp(SubsystemStatusResp&&) = default;
		SubsystemStatusResp& operator=(const SubsystemStatusResp&) = default;
		SubsystemStatusResp& operator=(SubsystemStatusResp&&) = default;

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
		[[nodiscard]] std::uint32_t getStatusSeqNum() const
		{
			return this->statusSeqNum;
		}
		void setStatusSeqNum(std::uint32_t status)
		{
			this->statusSeqNum = status;
		}
		[[nodiscard]] Failure getFailureLevel() const
		{
			return this->failureLevel;
		}
		void setFailureLevel(Failure fail)
		{
			this->failureLevel = fail;
		}
		[[nodiscard]] std::uint32_t getSubsystemCount() const
		{
			return this->subsystemCount;
		}
		void setSubsystemCount(std::uint32_t count)
		{
			this->subsystemCount = count;
		}
		[[nodiscard]] std::vector<SubsystemDepInfo> getSubsystems() const
		{
			return this->subsystems;
		}
		// replace the existing vector with a new vector
		void setSubsystems(const std::vector<SubsystemDepInfo>& subsys)
		{
			this->subsystems = subsys;
		}
		// add a new element to the vector
		void addSubsystemDepInfo(const SubsystemDepInfo& subsys)
		{
			this->subsystems.push_back(subsys);
		}
		[[nodiscard]] std::uint32_t getCsciCount() const
		{
			return this->csciCount;
		}
		void setCsciCount(std::uint32_t csciCount_in)
		{
			this->csciCount = csciCount_in;
		}
		[[nodiscard]] std::vector<SubsystemCSCIInfo> getCSCI() const
		{
			return this->csci;
		}
		// replace the existing vector with a new vector
		void setCSCI(const std::vector<SubsystemCSCIInfo>& info)
		{
			this->csci = info;
		}
		// add a new element to the vector
		void addSubsystemCSCIInfo(const SubsystemCSCIInfo& info)
		{
			this->csci.push_back(info);
		}

	private:
		std::uint32_t subsystemId{0};
		std::uint32_t criticality{0};
		std::uint32_t statusSeqNum{0};
		Failure failureLevel{Failure::NA};
		std::uint32_t subsystemCount{0};
		std::vector<SubsystemDepInfo> subsystems{};
		std::uint32_t csciCount{0};
		std::vector<SubsystemCSCIInfo> csci{};
	};

} // end namespace ams::iface::irmel
