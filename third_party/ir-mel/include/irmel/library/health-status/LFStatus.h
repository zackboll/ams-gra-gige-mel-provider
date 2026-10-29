//===============================================================================
/// @file  LFStatus.h
/// @brief This file includes LFStatus and associated enumeration definitions

#pragma once

#include <cstdint>
#include <string>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
/// Of interest is the Command Type enum class,
/// which documents all commands that can be sent from the MFP to the MFA.
/// The Classes in the file
/// describe how the connections between the MFA and MFP are made.
namespace ams::iface::irmel
{
	/// @enum LFStatusEnum
	/// @brief Indicates the reported status of a local function. Inspired by UCI LFStatusEnum
	/// @RequiredIfLFSupport This enumeration provides data definition associated with local functions
	///  and must be included as-is in all IR MEL implementations for IR MFAs that support local functions.
	enum class LFStatusEnum : std::uint32_t
	{
		Initializing, ///< Denotes that the Local Function is in an initialization state
		Normal,		  ///< Denotes that the local function is in a normal state
		Degraded	  ///< Denotes that the local function is in a degraded state
	};

	/// @enum FaultEnum
	/// @brief Indicates the type of fault reported by a local function. Inspired by UCI Fault
	/// @RequiredIfLFSupport This enumeration provides data definition associated with local functions
	///  and must be included as-is in all IR MEL implementations for IR MFAs that support local functions.
	enum class FaultEnum : std::uint32_t
	{
		None,	 ///< Denotes no fault
		Caution, ///< Denotes a minor fault
		Warning, ///< Denotes a major fault
		Failed	 ///< Denotes a severe fault
	};

	/// @class LFStatus
	/// @brief Reports the status of a Local Function.
	/// @RequiredIfLFSupport This class provides data definition associated with local functions
	///  and must be included as-is in all IR MEL implementations for IR MFAs that support local functions.
	class LFStatus
	{
	public:
		LFStatus() = default;
		~LFStatus() = default;
		LFStatus(const LFStatus&) = default;
		LFStatus(LFStatus&&) = default;
		LFStatus& operator=(const LFStatus&) = default;
		LFStatus& operator=(LFStatus&&) = default;

		[[nodiscard]] const std::string& getApertureConfigID() const
		{
			return this->apertureConfigID;
		}
		void setApertureConfigID(const std::string& apertureConfigID_in)
		{
			this->apertureConfigID = apertureConfigID_in;
		}
		[[nodiscard]] std::uint32_t getLFID() const
		{
			return this->LFID;
		}
		void setLFID(const std::uint32_t& LFID_in)
		{
			this->LFID = LFID_in;
		}
		[[nodiscard]] const LFStatusEnum& getLFStatus() const
		{
			return this->lfStatus;
		}
		void setLFStatus(const LFStatusEnum& lfStatus_in)
		{
			this->lfStatus = lfStatus_in;
		}
		[[nodiscard]] const FaultEnum& getFault() const
		{
			return this->fault;
		}
		void setFault(const FaultEnum& fault_in)
		{
			this->fault = fault_in;
		}

	private:
		std::string apertureConfigID{};
		std::uint32_t LFID{0};												  // ID for the local function
		LFStatusEnum lfStatus{ams::iface::irmel::LFStatusEnum::Initializing}; // Enum inspired from the UCI LFStatusEnum with values of
																			  // Initializing, Normal, Degraded
		FaultEnum fault{ams::iface::irmel::FaultEnum::None}; // Enum inspired from UCI Fault definition with values of None, Caution, Warning, Failed
	};
} // end namespace ams::iface::irmel
