#pragma once

#include <vector>
#include "NameValuePair.h"

namespace ams::iface::mel
{
	/// @brief Reports subsystem unique data that cannot be reported with other messages. This
	/// message is intended for use specifically with MFA Resource/Hardware metrics and implementers
	/// that use this message should fully document namepair values.
	/// @Optional This class provides data definition associated with status reporting
	/// and expected values documented in all RF MEL implementations that use this class. This
	/// class provides a method for reporting metrics related to the MFA's current status that
	/// are captured as a namepair for instance "MFA_COMPUTE_MEM_USAGE" "23".
	/// @class MFA_StatusDetailed
	class MFA_StatusDetailed
	{
	public:
		MFA_StatusDetailed() = default;
		explicit MFA_StatusDetailed(std::vector<NameValuePair>& s) : status{s}
		{
		}
		~MFA_StatusDetailed() = default;
		MFA_StatusDetailed(const MFA_StatusDetailed&) = default;
		MFA_StatusDetailed(MFA_StatusDetailed&&) = default;
		MFA_StatusDetailed& operator=(const MFA_StatusDetailed&) = default;
		MFA_StatusDetailed& operator=(MFA_StatusDetailed&&) = default;

		/// @brief Returns the complete list of statuses.
		[[nodiscard]] const std::vector<NameValuePair>& getStatus() const
		{
			return this->status;
		}
		/// @brief Sets (or replaces) the existing detailed status list.
		void setStatus(const std::vector<NameValuePair>& newValue)
		{
			this->status = newValue;
		}
		/// @brief Adds an additional detailed status element to the list.
		void addStatus(const NameValuePair& nvp)
		{
			this->status.push_back(nvp);
		}

	private:
		std::vector<NameValuePair> status;
	};
} // namespace ams::iface::mel
