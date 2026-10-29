//===============================================================================
/// @file  SubsystemCSCIInfo.h
/// @brief This file includes the subsystem info available in an SubsystemStatusResp message.

#pragma once

#include <irmel/library/health-status/Version.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <cstdint>
#include <string>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class SubsystemCSCIInfo
	/// @brief Gathers information of the configuration of a subsystem
	/// @Required This class prvoides data definition in support of required IR MEL functionality to report configuration info
	/// must be included as-is in all IR MEL implementations
	class SubsystemCSCIInfo
	{
	public:
		SubsystemCSCIInfo() = default;
		~SubsystemCSCIInfo() = default;
		SubsystemCSCIInfo(const SubsystemCSCIInfo&) = default;
		SubsystemCSCIInfo(SubsystemCSCIInfo&&) = default;
		SubsystemCSCIInfo& operator=(const SubsystemCSCIInfo&) = default;
		SubsystemCSCIInfo& operator=(SubsystemCSCIInfo&&) = default;

		[[nodiscard]] const std::string& getCsci() const
		{
			return this->csci;
		}
		void setCsci(const std::string& csci_in)
		{
			this->csci = csci_in;
		}
		[[nodiscard]] CSCIMode getMode() const
		{
			return this->mode;
		}
		void setMode(CSCIMode mode_in)
		{
			this->mode = mode_in;
		}
		[[nodiscard]] Version getVersion() const
		{
			return this->version;
		}
		void setVersion(Version version_in)
		{
			this->version = version_in;
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
		[[nodiscard]] std::uint32_t getBIT_report() const
		{
			return this->BIT_report;
		}
		void setBIT_report(std::uint32_t BIT_report_in)
		{
			this->BIT_report = BIT_report_in;
		}
		[[nodiscard]] bool getConnectionEstablished() const
		{
			return this->connectionEstablished;
		}
		void setConnectionEstablished(bool connectionEstablished_in)
		{
			this->connectionEstablished = connectionEstablished_in;
		}

	private:
		std::string csci{};
		CSCIMode mode{0};
		Version version = Version(0, 0, 0, 0);
		std::uint32_t criticality{0};
		Failure failureLevel{0};
		/// Serves as a counter that increments when there is a change to BIT_Status
		/// and enables a pull pattern for getting BIT Statuses to a Service
		std::uint32_t BIT_report{0};
		bool connectionEstablished{false};
	};
} // namespace ams::iface::irmel
