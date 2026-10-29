#pragma once
#include <chrono>
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

#include "UCI_ID.h"

namespace ams::iface::mel
{
	/// @enum BIT_Result
	/// @brief Indicates the overall status of the Built In Test results for the subsystem.
	/// @Required This enumeration class defines data used in MEL function definitions to indicate BIT test results
	/// and must be included as-is in all MEL implementations.
	enum class BIT_Result : std::uint32_t
	{
		NotSet,		 /// < enum has not been set
		Pass,		 ///< Indicates the specific BIT test completed successfully.
		Fail,		 ///< Indicates the specific BIT test failed.
		Interrupted, ///< Indicates the specific BIT test was interrupted and was not completed.
		NotTested,	 ///< Indicates the specific BIT test was not executed.
		MaxExclusive ///< maximun enum item
	};

	/// @class CompletedBIT_Item
	/// @brief This type is aligned with the OMS/ UCI SubsystemCompletedBIT_ItemType.
	/// @Required This class provides data definition in support of required MEL functionality to report BIT tests
	/// and must be included as-is in all MEL implementations.
	class CompletedBIT_Item
	{
	public:
		CompletedBIT_Item() = default;
		CompletedBIT_Item(std::string bit, BIT_Result r, std::string fail) : bitItemName{std::move(bit)}, result{r}, failReason{std::move(fail)}
		{
		}
		~CompletedBIT_Item() = default;
		CompletedBIT_Item(const CompletedBIT_Item&) = default;
		CompletedBIT_Item(CompletedBIT_Item&&) = default;
		CompletedBIT_Item& operator=(const CompletedBIT_Item&) = default;
		CompletedBIT_Item& operator=(CompletedBIT_Item&&) = default;

		[[nodiscard]] const std::string& getBitItemName() const
		{
			return this->bitItemName;
		}

		void setBitItemName(const std::string& newValue)
		{
			this->bitItemName = newValue;
		}

		[[nodiscard]] const BIT_Result& getResult() const
		{
			return this->result;
		}

		void setResult(BIT_Result newValue)
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
		/// Indicates the name of an item tested by the BIT.  BIT items can be logical or
		/// physical, singular or aggregated.
		std::string bitItemName;
		BIT_Result result{BIT_Result::NotSet}; ///< Indicates the BIT result for the tested item.  See BIT_Result.
		/// Indicates a human readable reason why the test of the item failed.
		/// This element is only expected when the sibling Result element indicates the test of the item failed.
		std::string failReason;
	};

	/// @class CompletedBIT
	/// @brief This type is aligned with the OMS/ UCI SubsystemCompletedBIT_Type.
	/// @Required This class provides data definition in support of required MEL functionality to report BIT tests
	/// and must be included as-is in all MEL implementations.
	class CompletedBIT
	{
	public:
		CompletedBIT() = default;
		CompletedBIT(UCI_ID id, std::chrono::nanoseconds tag, BIT_Result r, std::string fail, std::vector<CompletedBIT_Item> item)
			: bitID{std::move(id)}, timeTag{tag}, result{r}, failReason{std::move(fail)}, bitItem{std::move(item)}
		{
		}
		~CompletedBIT() = default;
		CompletedBIT(const CompletedBIT&) = default;
		CompletedBIT(CompletedBIT&&) = default;
		CompletedBIT& operator=(const CompletedBIT&) = default;
		CompletedBIT& operator=(CompletedBIT&&) = default;

		[[nodiscard]] const UCI_ID& getBitID() const
		{
			return this->bitID;
		}

		void setBitID(const UCI_ID& newValue)
		{
			this->bitID = newValue;
		}

		[[nodiscard]] std::chrono::nanoseconds getTimeTag() const
		{
			return this->timeTag;
		}

		void setTimeTag(std::chrono::nanoseconds newValue)
		{
			this->timeTag = newValue;
		}

		[[nodiscard]] const BIT_Result& getResult() const
		{
			return this->result;
		}

		void setResult(BIT_Result newValue)
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

		[[nodiscard]] const std::vector<CompletedBIT_Item>& getBitItem() const
		{
			return this->bitItem;
		}

		// replace the existing vector with a new vector
		void setBitItem(const std::vector<CompletedBIT_Item>& newValue)
		{
			this->bitItem = newValue;
		}

		// add a new element to the vector
		void addCompletedBIT_Item(const CompletedBIT_Item& item)
		{
			this->bitItem.push_back(item);
		}

	private:
		UCI_ID bitID;						 ///< unique ID
		std::chrono::nanoseconds timeTag{0}; ///< when the BIT ended, in nanoseconds UTC
		/// Indicates the overall summary result of the BIT.  Results of individual items included
		/// in the BIT are given in the sibling BIT_Item element.  See enumeration annotations for further details.
		BIT_Result result{BIT_Result::NotSet};
		/// Indicates a human readable reason why the BIT failed.  This element is only expected
		/// when the sibling Result element indicates the BIT failed.
		std::string failReason;
		std::vector<CompletedBIT_Item> bitItem; ///< Indicates the results of an item tested by the BIT.
	};
} // end namespace ams::iface::mel
