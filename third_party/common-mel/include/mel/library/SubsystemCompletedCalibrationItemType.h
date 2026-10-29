#pragma once

#include <string>
#include <cstdint>
#include <utility>
#include "CommonMEL.h"

using namespace ams::iface::mel;

namespace ams::iface::mel
{
	/// @brief Indicates the Calibration result for the item.
	/// @Required This enumeration provides data definition associated with system settings
	/// and must be included as-is in all RF MEL implementations.
	enum class SubsystemCalibrationResult : uint32_t
	{
		NotSet, /// enum has not been set
		Completed,
		Failed,
		Interrupted,
		NotCalibrated,
		MaxExclusive /// maximun enum item

	};

	/// @brief Indicates the results of an item tested by the Calibration.
	/// @Required This class provides data definition associated with status reporting
	/// and must be included as-is in all RF MEL implementations.
	class SubsystemCompletedCalibrationItemType
	{
	public:
		SubsystemCompletedCalibrationItemType() = default;
		SubsystemCompletedCalibrationItemType(std::string item, std::string fail) : itemName{std::move(item)}, failReason{std::move(fail)}
		{
		}
		~SubsystemCompletedCalibrationItemType() = default;
		SubsystemCompletedCalibrationItemType(const SubsystemCompletedCalibrationItemType&) = default;
		SubsystemCompletedCalibrationItemType(SubsystemCompletedCalibrationItemType&&) = default;
		SubsystemCompletedCalibrationItemType& operator=(const SubsystemCompletedCalibrationItemType&) = default;
		SubsystemCompletedCalibrationItemType& operator=(SubsystemCompletedCalibrationItemType&&) = default;

		[[nodiscard]] const std::string& getItemName() const
		{
			return this->itemName;
		}
		void setItemName(const std::string& newValue)
		{
			this->itemName = newValue;
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

	private:
		std::string itemName;
		SubsystemCalibrationResult result{SubsystemCalibrationResult::NotSet};
		std::string failReason;
	};
} // namespace ams::iface::mel
