//===============================================================================
/// @file  Version.h
/// @brief This file includes the subsystem version, used in subsystem info.

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class Version
	/// @brief Indicates the version of report generated of the Subsystem
	/// @Required This class provides data definition in support of required IR MEL functionality to indicate version number
	/// and must be included as-is in all IR MEL implementations
	class Version
	{
	public:
		Version() = default;
		Version(std::uint32_t so, std::uint32_t majR, std::uint32_t minR, std::uint32_t eR)
			: source{so}, majorRevision{majR}, minorRevision{minR}, engineeringRevision{eR}
		{
		}
		~Version() = default;
		Version(const Version&) = default;
		Version(Version&&) = default;
		Version& operator=(const Version&) = default;
		Version& operator=(Version&&) = default;

		[[nodiscard]] std::uint32_t getSource() const
		{
			return this->source;
		}
		void setSource(std::uint32_t source_in)
		{
			this->source = source_in;
		}
		[[nodiscard]] std::uint32_t getMajorRevision() const
		{
			return this->majorRevision;
		}
		void setMajorRevision(std::uint32_t majorRevision_in)
		{
			this->majorRevision = majorRevision_in;
		}
		[[nodiscard]] std::uint32_t getMinorRevision() const
		{
			return this->minorRevision;
		}
		void setMinorRevision(std::uint32_t minorRevision_in)
		{
			this->minorRevision = minorRevision_in;
		}
		[[nodiscard]] std::uint32_t getEngineeringRevision() const
		{
			return this->engineeringRevision;
		}
		void setEngineeringRevision(std::uint32_t engineeringRevision_in)
		{
			this->engineeringRevision = engineeringRevision_in;
		}

	private:
		std::uint32_t source{0};
		std::uint32_t majorRevision{0};
		std::uint32_t minorRevision{0};
		std::uint32_t engineeringRevision{0};
	};
} // namespace ams::iface::irmel
