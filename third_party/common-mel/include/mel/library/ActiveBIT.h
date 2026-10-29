#pragma once

#include <chrono>
#include <utility>
#include "UCI_ID.h"

namespace ams::iface::mel
{
	/// @class ActiveBIT
	/// @brief This type is aligned with the OMS/ UCI SubsystemActiveBIT_Type.
	/// @Required This class provides data definition in support of required MEL functionality to provide BIT stauts
	/// and must be included as-is in all MEL implementations.
	class ActiveBIT
	{
	public:
		ActiveBIT() = default;
		ActiveBIT(UCI_ID id, std::chrono::nanoseconds ect, double epc)
			: bitID{std::move(id)}, estimatedCompletionTime{ect}, estimatedPercentComplete{epc}
		{
		}
		~ActiveBIT() = default;
		ActiveBIT(const ActiveBIT&) = default;
		ActiveBIT(ActiveBIT&&) = default;
		ActiveBIT& operator=(const ActiveBIT&) = default;
		ActiveBIT& operator=(ActiveBIT&&) = default;

		[[nodiscard]] const UCI_ID& getBitID() const
		{
			return this->bitID;
		}

		void setBitID(const UCI_ID& newValue)
		{
			this->bitID = newValue;
		}

		[[nodiscard]] std::chrono::nanoseconds getEstimatedCompletionTime() const
		{
			return this->estimatedCompletionTime;
		}

		void setEstimatedCompletionTime(std::chrono::nanoseconds newValue)
		{
			this->estimatedCompletionTime = newValue;
		}

		[[nodiscard]] double getEstimatedPercentComplete() const
		{
			return this->estimatedPercentComplete;
		}

		void setEstimatedPercentComplete(double newValue)
		{
			this->estimatedPercentComplete = newValue;
		}

	private:
		UCI_ID bitID;										 ///< unique ID
		std::chrono::nanoseconds estimatedCompletionTime{0}; ///< nanoseconds UTC. This field is optional, it can be empty.
		double estimatedPercentComplete{0};					 ///< 100% = 1.0. Values greater than 100% are allowed. This
															 ///< field is optional, it can be empty.
	};
} // end namespace ams::iface::mel
