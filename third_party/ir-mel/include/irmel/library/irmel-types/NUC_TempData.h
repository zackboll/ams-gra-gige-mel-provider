//===============================================================================
/// @file  NUC_TempData.h
/// @brief This file includes the NUC_TempData type.
//===============================================================================
#pragma once

#include <vector>

#include <irmel/library/irmel-types/TempPoint.h>

namespace ams::iface::irmel
{
	/// @class NUC_TempData
	/// @brief Metadata item to read out calibration data from NUC process
	/// @Optional For MFAs that perform NUC, this class will provide a method to read NUC calibration data
	class NUC_TempData
	{
	public:
		NUC_TempData() = default;
		explicit NUC_TempData(const std::vector<TempPoint>& tempPoints_in) : tempPoints{tempPoints_in}
		{
		}
		~NUC_TempData() = default;
		NUC_TempData(const NUC_TempData&) = default;
		NUC_TempData(NUC_TempData&&) = default;
		NUC_TempData& operator=(const NUC_TempData&) = default;
		NUC_TempData& operator=(NUC_TempData&&) = default;

		[[nodiscard]] std::vector<TempPoint> getTempPoints() const
		{
			return this->tempPoints;
		}
		void setTempPoints(const std::vector<TempPoint>& tempPoints_in)
		{
			this->tempPoints = tempPoints_in;
		}
		void addTempPoint(TempPoint pointToAdd)
		{
			this->tempPoints.push_back(pointToAdd);
		}

	private:
		/// Stores a list of TempPoint objects which describe how the pixel values of the
		/// MFA's imagery correlate to temperature. These points are typically based on
		/// data collected from a NUC calibration.
		std::vector<TempPoint> tempPoints;
	};
} // end namespace ams::iface::irmel
