#pragma once

#include <vector>
#include "CommonMEL.h"
#include "SubsystemCompletedCalibration.h"

namespace ams::iface::mel
{
	/// @brief Indicates the current status and results of Subsystem Calibration.
	/// @Required This class provides data definition associated with status reporting
	/// and must be included as-is in all RF MEL implementations.
	class CalibrationStatus
	{
	public:
		CalibrationStatus() = default;
		CalibrationStatus(std::vector<UCI_ID>& active, std::vector<SubsystemCompletedCalibration>& completed)
			: activeCalibrationIDs{active}, completedCalibrations{completed}
		{
		}
		~CalibrationStatus() = default;
		CalibrationStatus(const CalibrationStatus&) = default;
		CalibrationStatus(CalibrationStatus&&) = default;
		CalibrationStatus& operator=(const CalibrationStatus&) = default;
		CalibrationStatus& operator=(CalibrationStatus&&) = default;

		[[nodiscard]] const std::vector<UCI_ID>& getActiveCalibrationIDs() const
		{
			return this->activeCalibrationIDs;
		}
		// replace the existing vector with a new vector
		void setActiveCalibrationIDs(const std::vector<UCI_ID>& newValue)
		{
			this->activeCalibrationIDs = newValue;
		}
		// add a new element to the vector
		void addActiveCalibrationID(const UCI_ID& id)
		{
			this->activeCalibrationIDs.push_back(id);
		}
		[[nodiscard]] const std::vector<SubsystemCompletedCalibration>& getCompletedCalibrations() const
		{
			return this->completedCalibrations;
		}
		// replace the existing vector with a new vector
		void setCompletedCalibrations(const std::vector<SubsystemCompletedCalibration>& newValue)
		{
			this->completedCalibrations = newValue;
		}
		// add a new element to the vector
		void addCompletedCalibration(const SubsystemCompletedCalibration& scc)
		{
			this->completedCalibrations.push_back(scc);
		}

	private:
		/// @brief Indicates the unique ID of the Subsystem whose Calibration status is being reported.
		std::vector<UCI_ID> activeCalibrationIDs;
		/// @brief Indicates results of a previously completed Calibration.
		std::vector<SubsystemCompletedCalibration> completedCalibrations;
	};
} // namespace ams::iface::mel
