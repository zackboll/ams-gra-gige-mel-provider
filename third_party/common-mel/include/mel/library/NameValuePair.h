#pragma once
#include <string>
#include <utility>

namespace ams::iface::mel
{
	/// @class NameValuePair
	/// @brief Used to report the value of an attribute that is not defined by other types or structures.
	/// @Required This class provides data definition associated with UCI and must be included as-is in all RF MEL implementations.
	class NameValuePair
	{
	public:
		NameValuePair() = default;
		NameValuePair(std::string n, std::string v) : name{std::move(n)}, value{std::move(v)}
		{
		}
		~NameValuePair() = default;
		NameValuePair(const NameValuePair&) = default;
		NameValuePair(NameValuePair&&) = default;
		NameValuePair& operator=(const NameValuePair&) = default;
		NameValuePair& operator=(NameValuePair&&) = default;

		/// @brief Gets the name.
		[[nodiscard]] const std::string& getName() const
		{
			return this->name;
		}
		/// @brief Sets the name.
		void setName(const std::string& newValue)
		{
			this->name = newValue;
		}
		/// @brief Gets the value.
		[[nodiscard]] const std::string& getValue() const
		{
			return this->value;
		}
		/// @brief Sets the value.
		void setValue(const std::string& newValue)
		{
			this->value = newValue;
		}

	private:
		std::string name;
		std::string value;
	};
} // end namespace ams::iface::mel
