//===============================================================================
/// @file  HotRegion.h
/// @brief This file contains the HotRegion type definition

#pragma once

#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @enum HotRegionTypeEnum
	/// @brief Indicates a known high-intensity object within the FOV of a sensor, such as the sun.
	/// @Optional This enum provides a definition of hot region types in support of Candidate Objects
	enum HotRegionTypeEnum
	{
		HOTREGIONTYPE_INVALID = 0, ///< Indicates that a declared hot region is of invalid type
		HOTREGIONTYPE_FLARE = 1,   ///< Indicates that a declared hot region is a flare
		HOTREGIONTYPE_SOLAR = 2,   ///< Indicates that a declared hot region is a nearby star
		HOTREGIONTYPE_MASK = 3	   ///< Indicates that a declared hot region is inactive or masked
	};

	/// @class HotRegion
	/// @brief Class for declaring a high intensity object known to be within the FOV of a sensor
	/// @Optional This class describes a hot region for use in the CandidateObjectHeader
	class HotRegion
	{
	public:
		HotRegion() = default;
		HotRegion(HotRegionTypeEnum type_in, std::uint16_t size_in, std::uint16_t top_in, std::uint16_t left_in, std::uint16_t right_in, std::uint16_t bottom_in)
			: type{type_in}, size{size_in}, top{top_in}, left{left_in}, right{right_in}, bottom{bottom_in}
		{
		}
		~HotRegion() = default;
		HotRegion(const HotRegion&) = default;
		HotRegion(HotRegion&&) = default;
		HotRegion& operator=(const HotRegion&) = default;
		HotRegion& operator=(HotRegion&&) = default;

		[[nodiscard]] HotRegionTypeEnum getType() const
		{
			return this->type;
		}
		void setType(HotRegionTypeEnum type_in)
		{
			this->type = type_in;
		}
		[[nodiscard]] std::uint16_t getSize() const
		{
			return this->size;
		}
		void setSize(std::uint16_t size_in)
		{
			this->size = size_in;
		}
		[[nodiscard]] std::uint16_t getTop() const
		{
			return this->top;
		}
		void setTop(std::uint16_t top_in)
		{
			this->top = top_in;
		}
		[[nodiscard]] std::uint16_t getLeft() const
		{
			return this->left;
		}
		void setLeft(std::uint16_t left_in)
		{
			this->left = left_in;
		}
		[[nodiscard]] std::uint16_t getRight() const
		{
			return this->right;
		}
		void setRight(std::uint16_t right_in)
		{
			this->right = right_in;
		}
		[[nodiscard]] std::uint16_t getBottom() const
		{
			return this->bottom;
		}
		void setBottom(std::uint16_t bottom_in)
		{
			this->bottom = bottom_in;
		}

	private:
		HotRegionTypeEnum type{HotRegionTypeEnum::HOTREGIONTYPE_INVALID}; ///< Type of the hot region
		std::uint16_t size{0};											  ///< number of pixels that make up the hot region
		std::uint16_t top{0};											  ///< top pixel position with zero-based indexing
		std::uint16_t left{0};											  ///< left pixel position with zero-based indexing
		std::uint16_t right{0};											  ///< right pixel position with zero-based indexing
		std::uint16_t bottom{0};										  ///< bottom pixel position with zero-based indexing
	};

} // namespace ams::iface::irmel
