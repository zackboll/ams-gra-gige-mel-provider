#pragma once

#include <variant>
#include <optional>
#include <utility>

#include "Error.h"

namespace ams::iface::mel
{
	template <typename T>
	/// @Required This class provides data definition in support of required MEL functionality
	/// to provide error messaging/information and must be included as-is in MEL implementations.
	/// @brief A common return data type that mimic std::optional but with error codes and data.
	/// This is basically syntactic sugar around a variant.
	class ErrorOr
	{
	public:
		/// @brief The error case.
		explicit ErrorOr(Error error) : errorOrData(std::move(error))
		{
		}

		/// @brief The data case.
		explicit ErrorOr(T data) : errorOrData(std::move(data))
		{
		}

		/// @brief Gets an error, otherwise will throw an exception.
		[[nodiscard]] const Error& getError() const
		{
			return std::get<Error>(errorOrData);
		}

		/// @brief Gets the data, otherwise will throw an exception.
		[[nodiscard]] T& get()
		{
			return std::get<T>(errorOrData);
		}

		/// @brief the boolean operator is used to determines if the class variant holds the alternative \<T\>
		/// @return true if the \<typename T\> is the variant, false if Error is the variant
		explicit operator bool() const
		{
			return std::holds_alternative<T>(errorOrData);
		}

		/// @brief Dereference to get data.
		T& operator*()
		{
			return std::get<T>(errorOrData);
		}

		/// @brief Dereference to get data.
		T* operator->()
		{
			return &(std::get<T>(errorOrData));
		}

		/// @brief Cast to T if it exists.
		explicit operator T&()
		{
			return std::get<T>(errorOrData);
		}

	private:
		// Error codes and data.
		std::variant<Error, T> errorOrData;
	};
} // namespace ams::iface::mel
