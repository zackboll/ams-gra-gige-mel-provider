#pragma once

#include <chrono>
#include <string>
#include <utility>
#include <vector>
#include "CommonMEL.h"
#include "SubsystemCompletedCalibrationItemType.h"

namespace ams::iface::mel
{
	/// @brief Indicates the results of a previously completed Calibration.
	/// @Required This class provides data definition associated with status reporting
	/// and must be included as-is in all RF MEL implementations.
	class SubsystemCompletedCalibration
	{
	public:
		SubsystemCompletedCalibration() = default;
		SubsystemCompletedCalibration(UCI_ID& id, std::chrono::nanoseconds time, SubsystemCalibrationResult r, std::string fail,
									  std::vector<SubsystemCompletedCalibrationItemType>& cal)
			: calibrationID{id}, timeTag{time}, result{r}, failReason{std::move(fail)}, calibrationItems{cal}
		{
		}
		~SubsystemCompletedCalibration() = default;
		SubsystemCompletedCalibration(const SubsystemCompletedCalibration&) = default;
		SubsystemCompletedCalibration(SubsystemCompletedCalibration&&) = default;
		SubsystemCompletedCalibration& operator=(const SubsystemCompletedCalibration&) = default;
		SubsystemCompletedCalibration& operator=(SubsystemCompletedCalibration&&) = default;

		[[nodiscard]] const UCI_ID& getCalibrationID() const
		{
			return this->calibrationID;
		}
		void setCalibrationID(const UCI_ID& newValue)
		{
			this->calibrationID = newValue;
		}
		[[nodiscard]] std::chrono::nanoseconds getTimeTag() const
		{
			return this->timeTag;
		}
		void setTimeTag(std::chrono::nanoseconds newValue)
		{
			this->timeTag = newValue;
		}
		[[nodiscard]] const SubsystemCalibrationResult& getResult() const
		{
			return this->result;
		}
		void setResult(SubsystemCalibrationResult newValue)
		{
			this->result = newValue;
		}
		[[nodiscard]] const std::string& getFailReason() const
		{
			return this->failReason;
		}
		void setFailReason(const std::string& newValue)
		{
			this->failReason = newValue;
		}
		[[nodiscard]] const std::vector<SubsystemCompletedCalibrationItemType>& getCalibrationItems() const
		{
			return this->calibrationItems;
		}
		// replace the existing vector with a new vector
		void setCalibrationItems(const std::vector<SubsystemCompletedCalibrationItemType>& newValue)
		{
			this->calibrationItems = newValue;
		}
		// add a new element to the vector
		void addCalibrationItem(const SubsystemCompletedCalibrationItemType& ci)
		{
			this->calibrationItems.push_back(ci);
		}

	private:
		/// @brief Indicates the unique ID of the Calibration.  For Calibration initiated via SubsystemCalibrationCommand,
		/// the ID corresponds to a Calibration indicated in SubsystemCalibrationConfiguration.
		UCI_ID calibrationID;
		/// @brief Indicates when the Calibration ended.
		std::chrono::nanoseconds timeTag{0};
		/// @brief Indicates the overall summary result of the Calibration.
		SubsystemCalibrationResult result{SubsystemCalibrationResult::NotSet};
		/// @brief Indicates a human readable reason why the Calibration failed.
		std::string failReason;
		/// @brief Indicates the results of an item tested by the Calibration.
		std::vector<SubsystemCompletedCalibrationItemType> calibrationItems;
	};
} // namespace ams::iface::mel
