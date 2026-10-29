//===============================================================================
/// @file  BadPixel.h
/// @brief This file includes the BadPixel class

#pragma once

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class BadPixel
	/// @brief Used by BadPixelList
	/// @Required This class provides data definition in support of required IR MEL functionality to report bad pixels
	/// and must be included as-is in all IR MEL implementations
	class BadPixel
	{
	public:
		BadPixel() = default;
		BadPixel(std::uint32_t r, std::uint32_t c, BadPixelReason reas) : row{r}, col{c}, reason{reas}
		{
		}
		~BadPixel() = default;
		BadPixel(const BadPixel&) = default;
		BadPixel(BadPixel&&) = default;
		BadPixel& operator=(const BadPixel&) = default;
		BadPixel& operator=(BadPixel&&) = default;

		[[nodiscard]] std::uint32_t getRow() const
		{
			return this->row;
		}
		void setRow(std::uint32_t r)
		{
			this->row = r;
		}
		[[nodiscard]] std::uint32_t getCol() const
		{
			return this->col;
		}
		void setCol(std::uint32_t c)
		{
			this->col = c;
		}
		[[nodiscard]] BadPixelReason getReason() const
		{
			return this->reason;
		}
		void setReason(BadPixelReason reas)
		{
			this->reason = reas;
		}

	private:
		std::uint32_t row{0};
		std::uint32_t col{0};
		BadPixelReason reason{BadPixelReason::Unknown};
	};
} // namespace ams::iface::irmel
