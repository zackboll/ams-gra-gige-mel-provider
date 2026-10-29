#pragma once

#include <vector>
#include "CommonMEL.h"

namespace ams::iface::mel
{
	/// @class DiscreteStatus
	/// @brief Reports subsystem unique data that cannot be reported with other messages.
	/// @Required This class provides data definition associated with status reporting
	/// and must be included as-is in all MEL implementations.
	class DiscreteStatus
	{
	public:
		DiscreteStatus() = default;
		explicit DiscreteStatus(std::vector<NameValuePair>& s) : status{s}
		{
		}
		~DiscreteStatus() = default;
		DiscreteStatus(const DiscreteStatus&) = default;
		DiscreteStatus(DiscreteStatus&&) = default;
		DiscreteStatus& operator=(const DiscreteStatus&) = default;
		DiscreteStatus& operator=(DiscreteStatus&&) = default;

		[[nodiscard]] const std::vector<NameValuePair>& getStatus() const
		{
			return this->status;
		}
		void setStatus(const std::vector<NameValuePair>& newValue)
		{
			this->status = newValue;
		}

	private:
		std::vector<NameValuePair> status;
	};

} // end namespace ams::iface::mel
