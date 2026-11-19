//===============================================================================
/// @file ScanPerformanceReport.h
/// @brief This file includes the ScanPerformanceReport class definition
///==============================================================================

#pragma once

#include <irmel/library/irmel-types/ScanParam.h>

/// @namespace ams::iface::irmel
/// This namespace contains the ScanPerformancereport class definition
namespace ams::iface::irmel
{
	/// @class ScanPerformanceReport
	/// @brief The ScanPerformanceReport class provides additional information to services regarding a degraded scan
	/// @Optional This class supports additional scan information and is optional for IR MEL Implementations
	class ScanPerformanceReport
	{
	public:
		ScanPerformanceReport() = default;
		ScanPerformanceReport(ScanParam com, ScanParam ach) : commanded{com}, achieved{ach}
		{
		}
		~ScanPerformanceReport() = default;
		ScanPerformanceReport(const ScanPerformanceReport&) = default;
		ScanPerformanceReport(ScanPerformanceReport&&) = default;
		ScanPerformanceReport& operator=(const ScanPerformanceReport&) = default;
		ScanPerformanceReport& operator=(ScanPerformanceReport&&) = default;

		[[nodiscard]] ScanParam getCommanded() const
		{
			return this->commanded;
		}
		void setCommanded(ScanParam commanded_in)
		{
			this->commanded = commanded_in;
		}
		[[nodiscard]] ScanParam getAchieved() const
		{
			return this->achieved;
		}
		void setAchieved(ScanParam achieved_in)
		{
			this->achieved = achieved_in;
		}

	private:
		ScanParam commanded{}; ///< The commanded attributes of the scan
		ScanParam achieved{};  ///< The attributes of the scan performed
	};

} // end namespace ams::iface::irmel
