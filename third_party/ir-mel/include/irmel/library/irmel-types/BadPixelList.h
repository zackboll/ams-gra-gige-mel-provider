//===============================================================================
/// @file  BadPixelList.h
/// @brief This file includes the BadPixelList class

#pragma once

#include <cstdint>
#include <vector>

#include <irmel/library/irmel-types/BadPixel.h>

namespace ams::iface::irmel
{
	/// @class BadPixelList
	/// @brief Used to designate pixels that have some defect, which processing could mitigate
	/// @Required This class provides data definition in support of required IR MEL functionality to report defected pixels
	/// and must be included as-is in all IR MEL implementations
	class BadPixelList
	{
	public:
		BadPixelList() = default;
		BadPixelList(std::uint32_t s, std::uint32_t c, std::vector<BadPixel>& list) : size{s}, badPixelCount{c}, badPixelList{list}
		{
		}
		~BadPixelList() = default;
		BadPixelList(const BadPixelList&) = default;
		BadPixelList(BadPixelList&&) = default;
		BadPixelList& operator=(const BadPixelList&) = default;
		BadPixelList& operator=(BadPixelList&&) = default;

		[[nodiscard]] std::uint32_t getSize() const
		{
			return this->size;
		}
		void setSize(std::uint32_t s)
		{
			this->size = s;
		}
		[[nodiscard]] std::uint32_t getBadPixelCount() const
		{
			return this->badPixelCount;
		}
		void setBadPixelCount(std::uint32_t c)
		{
			this->badPixelCount = c;
		}
		// replace the existing vector with a new vector
		[[nodiscard]] const std::vector<BadPixel>& getBadPixelList() const
		{
			return this->badPixelList;
		}
		// replace existing vector
		void setBadPixelList(const std::vector<BadPixel>& list)
		{
			this->badPixelList = list;
		}
		// add a new element to the vector
		void addBadPixel(const BadPixel& pixel)
		{
			this->badPixelList.push_back(pixel);
		}

	private:
		std::uint32_t size{0};				  ///< Number of bytes in badPixelList
		std::uint32_t badPixelCount{0};		  ///< Number of bad pixels in the list.  The bad pixels are defined in badPixelList[0] through
											  ///< badPixelList[badPixelCount-1].
		std::vector<BadPixel> badPixelList{}; ///< Pointer to an array of bad pixel entries.
	};
} // namespace ams::iface::irmel
