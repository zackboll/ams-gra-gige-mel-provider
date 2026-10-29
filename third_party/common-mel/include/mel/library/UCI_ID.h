#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace ams::iface::mel
{
	constexpr size_t UUID_SIZE = 16;

	/// @class UCI_ID
	/// @brief The common base type for all UCI IDs.
	/// All extension types should end in "ID_Type".
	/// @Required This class provides data definition in support of required MEL functionality
	/// and must be included as-is in all MEL implementations.
	class UCI_ID
	{
	public:
		UCI_ID() = default;
		UCI_ID(const std::array<std::uint8_t, ams::iface::mel::UUID_SIZE>& id, std::string desc) : uuid{id}, descriptiveLabel{std::move(desc)}
		{
		}
		~UCI_ID() = default;
		UCI_ID(const UCI_ID&) = default;
		UCI_ID(UCI_ID&&) = default;
		UCI_ID& operator=(const UCI_ID&) = default;
		UCI_ID& operator=(UCI_ID&&) = default;
		bool operator==(const UCI_ID& rhs) const
		{
			return ((uuid == rhs.getUUID()) && (descriptiveLabel == rhs.getDescriptiveLabel()));
		};

		bool operator<(const UCI_ID& rhs) const
		{
			return ((uuid < rhs.getUUID()) && (descriptiveLabel < rhs.getDescriptiveLabel()));
		};

		[[nodiscard]] const std::array<std::uint8_t, ams::iface::mel::UUID_SIZE>& getUUID() const
		{
			return this->uuid;
		}

		void setUUID(const std::array<std::uint8_t, ams::iface::mel::UUID_SIZE>& newValue)
		{
			this->uuid = newValue;
		}

		[[nodiscard]] const std::string& getDescriptiveLabel() const
		{
			return this->descriptiveLabel;
		}

		void setDescriptiveLabel(const std::string& newValue)
		{
			this->descriptiveLabel = newValue;
		}

	private:
		/// Universally Unique Identifier (UUID).  A UUID is a 128-bit number (32 hexadecimal digits)
		/// standardized by the Open Software Foundation for the purpose of providing unique identifiers
		// in a distributed environment.
		std::array<std::uint8_t, ams::iface::mel::UUID_SIZE> uuid{0};
		std::string descriptiveLabel; ///< Human readable text label for operators
	};

	/// @class ForeignKey
	/// @brief  This element represents a key, ID, etc. that exists in another system,
	/// protocol, network, etc. This type is aligned with the OMS/ UCI ForeignKeyType.
	/// @Required This class provides data definition in support of required MEL functionality
	/// and must be included as-is in all MEL implementations.
	class ForeignKey
	{
	public:
		ForeignKey() = default;
		ForeignKey(std::string k, std::string sn) : key{std::move(k)}, systemName{std::move(sn)}
		{
		}
		~ForeignKey() = default;
		ForeignKey(const ForeignKey&) = default;
		ForeignKey(ForeignKey&&) = default;
		ForeignKey& operator=(const ForeignKey&) = default;
		ForeignKey& operator=(ForeignKey&&) = default;

		[[nodiscard]] const std::string& getKey() const
		{
			return this->key;
		}

		void setKey(const std::string& newValue)
		{
			this->key = newValue;
		}

		[[nodiscard]] const std::string& getSystemName() const
		{
			return this->systemName;
		}

		void setSystemName(const std::string& newValue)
		{
			this->systemName = newValue;
		}

	private:
		std::string key;
		/// This element refers to the "system" where the ForeignKey is managed.
		/// That system could be another message standard/ protocol, network, database, shared table, etc.
		/// UCI leaves it to implementers to choose and coordinate use of the foreign "system".
		std::string systemName;
	};
} // end namespace ams::iface::mel
