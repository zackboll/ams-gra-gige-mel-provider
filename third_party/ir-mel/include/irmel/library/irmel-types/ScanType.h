//===============================================================================
/// @file  ScanType.h
/// @brief This file includes the type of scan in a scan param message.

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class ScanType
	/// @brief Identifies type of scan used for tasks
	/// @Required  This class provides data definition in support of required IR MEL functionality to report scan type
	/// and must be included as-is in all IR MEL implementations, not all scan types need to be supported by a given MEL
	class ScanType
	{
	public:
		ScanType() = default;
		~ScanType() = default;
		ScanType(const ScanType&) = default;
		ScanType(ScanType&&) = default;
		ScanType& operator=(const ScanType&) = default;
		ScanType& operator=(ScanType&&) = default;

		/// @brief continuousscan refers to a single bar scan that moves back and forth across the FOV
		[[nodiscard]] std::uint32_t getContinuousScan() const
		{
			return this->continuousScan;
		}
		void setContinuousScan(std::uint32_t continuousScan_in)
		{
			this->continuousScan = continuousScan_in;
		}
		/// @brief returning provides the feedback status to continuousscan scan command
		[[nodiscard]] std::uint32_t getReturning() const
		{
			return this->returning;
		}
		/// @brief returning is used in conjunction with continuousScan to disambiguate the direction of scanning (i.e. low to high and back or high
		/// to low and back)
		void setReturning(std::uint32_t returning_in)
		{
			this->returning = returning_in;
		}
		/// @brief agileScan supports scan of subsection of the FOS, before moving to a following subsection, governed by prioritization of tracks
		[[nodiscard]] std::uint32_t getAgileScan() const
		{
			return this->agileScan;
		}
		void setAgileScan(std::uint32_t agileScan_in)
		{
			this->agileScan = agileScan_in;
		}

	private:
		std::uint32_t continuousScan{0}; ///< single bar scan
		std::uint32_t returning{0};		 ///< used in conjunction with continuousScan to disambiguate the direction of scanning (i.e. low to high and
										 ///< back or high to low and back)
		std::uint32_t agileScan{0}; ///< scan type that performs data collect on a subsection on the FOS, before moving to a following subsection,
									///< governed by prioritization of tracks
	};
} // namespace ams::iface::irmel
