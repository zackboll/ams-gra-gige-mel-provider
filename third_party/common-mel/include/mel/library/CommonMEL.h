#pragma once
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#include "BIT_Configuration.h"
#include "BIT_Status.h"
#include "ErrorOr.h"
#include "MFA_Component.h"
#include "MFA_StatusDetailed.h"
#include "NameValuePair.h"
#include "NavigationReport.h"
#include "securityauditrecord/MFA_SecurityAuditRecord.h"

/**
 * @(#) CmnMEL.idl
 */

namespace ams::iface::mel
{
	const std::string CMN_MEL_API_VERSION = "4.0";

	/// @class VersionInfo
	/// @brief Information about the library used in Capability
	/// @Required This class provides data definition in support of required MEL functionality to
	/// report versions of API and libraries, and it must be included as-is in all MEL implementations.
	class VersionInfo
	{
	public:
		VersionInfo() = default;
		VersionInfo(uint32_t apiVer, uint32_t libVer, std::string ven, std::string desc)
			: apiVersion{apiVer}, libVersion{libVer}, vendor{std::move(ven)}, description{std::move(desc)}
		{
		}
		~VersionInfo() = default;
		VersionInfo(const VersionInfo&) = default;
		VersionInfo(VersionInfo&&) = default;
		VersionInfo& operator=(const VersionInfo&) = default;
		VersionInfo& operator=(VersionInfo&&) = default;

		// Get and Set for the apiVersion
		[[nodiscard]] uint32_t getAPIVersion() const
		{
			return this->apiVersion;
		}

		void setAPIVersion(uint32_t newValue)
		{
			this->apiVersion = newValue;
		}

		// Get and Set for the libVerson
		[[nodiscard]] uint32_t getLibVersion() const
		{
			return this->libVersion;
		}

		void setLibVersion(uint32_t newValue)
		{
			this->libVersion = newValue;
		}

		// Get and Set for the vendor
		[[nodiscard]] const std::string& getVendor() const
		{
			return this->vendor;
		}

		void setVendor(const std::string& newValue)
		{
			this->vendor = newValue;
		}

		// Get and Set for the description
		[[nodiscard]] const std::string& getDescription() const
		{
			return this->description;
		}

		void setDescription(const std::string& newValue)
		{
			this->description = newValue;
		}

	private:
		uint32_t apiVersion{0};
		uint32_t libVersion{0};
		std::string vendor;
		std::string description;
	};

	/// @class UnimplementedException
	/// @brief class used as stub implementation to deprecated virtual functions in IR and RF MELs
	/// @Optional This class supports stub implementation and is Optional in all MEL implementations
	class UnimplementedException : public std::exception
	{
	public:
		UnimplementedException() = default;
		~UnimplementedException() = default;
		UnimplementedException(const UnimplementedException&) = default;
		UnimplementedException(UnimplementedException&&) = default;
		UnimplementedException& operator=(const UnimplementedException&) = default;
		UnimplementedException& operator=(UnimplementedException&&) = default;
		[[nodiscard]] const char* what() const noexcept override
		{
			return "Function not implemented.";
		}
	};

	/// @brief This template type represents a remote procedure call (RPC)
	/// requesting access to an object of 'T'.
	/// - The future<> holds the reference to the object which will receive the
	///	  result of the RPC, for which the application can wait or poll as needed.
	/// - The ErrorOr<> indicates the status of the request. On success, it holds
	///   the request object; on failure, it holds the resulting error code.
	/// - The result of a successful request is held by a shared_ptr<T>.
	///   When the reference counter of the shared_ptr reaches zero, the
	///   resource is automatically relinquished.
	template <typename T>
	using RequestFor = std::future<ErrorOr<std::shared_ptr<T>>>;

} // end namespace ams::iface::mel
