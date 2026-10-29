#pragma once

#include <utility>

#include <string>

namespace ams::iface::mel
{
	/// @brief Indicates why a request was not granted.
	/// @Required This enumeration provides data definition in support of required MEL functionality
	/// to provide error messaging/information and must be included as-is in MEL implementations.
	enum class ErrorCode
	{
		None,
		InvalidId,
		InvalidState,
		InvalidParameters,
		InsufficientPermissions,
		InsufficientResources,
		InsufficientLocalResources,
		InsufficientRemoteResources,
		Unsupported
	};

	/// @brief Represents a MEL error.
	/// @Required This class provides data definition in support of required MEL functionality
	/// to provide error messaging/information and must be included as-is in MEL implementations.
	class Error
	{
	public:
		Error() = delete;

		/// @brief Initialize the error with an error code and optionally a description.
		/// @param description Additional information on the Error in a string format
		/// @param code The error code
		explicit inline Error(ErrorCode code_in, std::string description_in = "") : code(code_in), description(std::move(description_in))
		{
		}
		~Error() = default;
		Error(const Error&) = default;
		Error(Error&& other) noexcept
		{
			this->code = std::move(other.code);
			this->description = std::move(other.description);
		}
		Error& operator=(const Error&) = default;
		Error& operator=(Error&& other) noexcept
		{
			if(this != &other)
			{
				this->code = std::move(other.code);
				this->description = std::move(other.description);
			}
			return *this;
		}

		/// @brief Get the description.
		/// @return The description.
		[[nodiscard]] inline const std::string& getDescription() const
		{
			return description;
		}

		/// @brief Get the error code.
		/// @return The error code.
		[[nodiscard]] inline ErrorCode getCode() const
		{
			return code;
		}

	private:
		//! The error code
		ErrorCode code{ErrorCode::None};

		//! The description of the error
		std::string description{};
	};
} // namespace ams::iface::mel
