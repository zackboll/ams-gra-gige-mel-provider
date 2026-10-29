#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "CommonMEL.h"

namespace ams::iface::mel
{
	/// @brief This enumerated type indicates the C2 messages and control
	/// flow patterns for Subsystem Calibration.
	/// @Required This enumeration provides data definition associated with system settings
	/// and must be included as-is in all RF MEL implementations.
	enum class SubsystemCalibrationControlInterfaces : uint32_t
	{
		NotSet, /// enum has not been set
		SubsystemCalibrationCommand,
		SubsystemStateCommand,
		MaxExclusive /// maximun enum item
	};

	/// @brief Indicates a Calibration supported by the Subsystem.
	/// @Required This class provides data definition associated with status reporting
	/// and must be included as-is in all RF MEL implementations.
	class SubsystemCalibration
	{
	public:
		SubsystemCalibration() = default;
		SubsystemCalibration(UCI_ID& cal, SubsystemCalibrationControlInterfaces acc, std::vector<std::string>& name, std::vector<UCI_ID>& subs,
							 std::vector<UCI_ID>& capa)
			: calibrationID{cal}, acceptedInterface{acc}, calibrationItemName{name}, subsystemComponentID{subs}, capabilityID{capa}
		{
		}
		~SubsystemCalibration() = default;
		SubsystemCalibration(const SubsystemCalibration&) = default;
		SubsystemCalibration(SubsystemCalibration&&) = default;
		SubsystemCalibration& operator=(const SubsystemCalibration&) = default;
		SubsystemCalibration& operator=(SubsystemCalibration&&) = default;

		[[nodiscard]] const UCI_ID& getCalibrationID() const
		{
			return this->calibrationID;
		}
		void setCalibrationID(const UCI_ID& newValue)
		{
			this->calibrationID = newValue;
		}
		[[nodiscard]] const SubsystemCalibrationControlInterfaces& getAcceptedInterface() const
		{
			return this->acceptedInterface;
		}
		void setAcceptedInterface(SubsystemCalibrationControlInterfaces newValue)
		{
			this->acceptedInterface = newValue;
		}
		[[nodiscard]] const std::vector<std::string>& getCalibrationItemName() const
		{
			return this->calibrationItemName;
		}
		// replace the existing vector with a new vector
		void setCalibrationItemName(const std::vector<std::string>& newValue)
		{
			this->calibrationItemName = newValue;
		}
		// add a new element to the vector
		void addCalibrationItemName(const std::string& item)
		{
			this->calibrationItemName.push_back(item);
		}
		[[nodiscard]] const std::vector<UCI_ID>& getSubsystemComponentID() const
		{
			return this->subsystemComponentID;
		}
		// replace the existing vector with a new vector
		void setSubsystemComponentID(const std::vector<UCI_ID>& newValue)
		{
			this->subsystemComponentID = newValue;
		}
		// add a new element to the vector
		void addSubsystemComponentID(const UCI_ID& id)
		{
			this->subsystemComponentID.push_back(id);
		}
		[[nodiscard]] const std::vector<UCI_ID>& getCapabilityID() const
		{
			return this->capabilityID;
		}
		// replace the existing vector with a new vector
		void setCapabilityID(const std::vector<UCI_ID>& newValue)
		{
			this->capabilityID = newValue;
		}
		// add a new element to the vector
		void addCapabilityID(const UCI_ID& id)
		{
			this->capabilityID.push_back(id);
		}

	private:
		/// @brief Indicates the unique ID of the Calibration along with its human readable name.
		UCI_ID calibrationID;
		/// @brief Indicates the calibration messaging interface that is supported by the Calibration.
		SubsystemCalibrationControlInterfaces acceptedInterface{SubsystemCalibrationControlInterfaces::NotSet};
		/// @brief Indicates the name of a non-Component non-Capability item included in the Calibration.
		std::vector<std::string> calibrationItemName;
		/// @brief Indicates a Component that is included in the Calibration.
		std::vector<UCI_ID> subsystemComponentID;
		/// @brief Indicates a Capability that is included in the Calibration.
		std::vector<UCI_ID> capabilityID;
	};

	/// @brief Indicates the Calibration messaging interfaces and specific calibrations supported by a Subsystem.
	using CalibrationConfiguration = std::vector<SubsystemCalibration>;

} // namespace ams::iface::mel
