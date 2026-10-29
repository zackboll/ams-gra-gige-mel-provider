//===============================================================================
/// @file  BandInfo.h
/// @brief This file includes the Band Info type.
//===============================================================================
#pragma once

#include <cstdint>

namespace ams::iface::irmel
{
	/// @enum BandType
	/// wavelengths for each enum item.
	/// @Required This class provides data definitions in support or required IR MEL functionality to describe the EM band
	/// of an image and must be included as-is in all IR MEL implementations
	enum class BandType : uint8_t
	{
		Invalid,	   ///< An invalid band_type - a default value
		Multiband,	   ///< Band representing simultaneous frequency spectrums
		IR_Far,		   ///< Band representing Far IR
		IR_Near,	   ///< Band representing Near IR
		IR_Longwave,   ///< Band representing longwave IR
		IR_Midwave,	   ///< Band representing midwave IR
		IR_Shortwave,  ///< Band representing shortwave IR
		Visible_White, ///< Band representing white light in the visible spectrum
		Visible_Red,   ///< Band representing red light in the visible spectrum
		Visible_Green, ///< Band representing green light in the visible spectrum
		Visible_Blue,  ///< Band representing blue light in the visible spectrum
		UVA,		   ///< Band representing A band in the ultraviolet spectrum
		UVB,		   ///< Band representing B band in the ultraviolet spectrum
		UVC,		   ///< Band representing C band in the ultraviolet spectrum
		UV_Vacuum	   ///< Band representing ultraviolet spectrum in a vacuum
	};

	/// @class BandInfo
	/// @brief Describes the EM band of a given image at a glance
	/// @Required This class provides data definitions in support or required IR MEL functionality to describe the EM band
	/// of an image and must be included as-is in all IR MEL implementations
	class BandInfo
	{
	public:
		BandInfo() = default;
		BandInfo(BandType type, double min, double max) : bandType{type}, minWavelength{min}, maxWavelength{max}
		{
		}
		~BandInfo() = default;
		BandInfo(const BandInfo&) = default;
		BandInfo(BandInfo&&) = default;
		BandInfo& operator=(const BandInfo&) = default;
		BandInfo& operator=(BandInfo&&) = default;

		[[nodiscard]] BandType getBandType() const
		{
			return this->bandType;
		}
		void setBandType(BandType type)
		{
			this->bandType = type;
		}

		[[nodiscard]] double getMinWavelength() const
		{
			return this->minWavelength;
		}
		void setMinWavelength(double min)
		{
			this->minWavelength = min;
		}

		[[nodiscard]] double getMaxWavelength() const
		{
			return this->maxWavelength;
		}
		void setMaxWavelength(double max)
		{
			this->maxWavelength = max;
		}

	private:
		BandType bandType{BandType::Invalid}; ///< Enum to describe overall place in EM spectrum
		double minWavelength{0};			  ///< Minimum wavelength of the band as measured in a vacuum in meters
		double maxWavelength{0};			  ///< Maximum wavelength of the band as measured in a vacuum in meters
	};

} // end namespace ams::iface::irmel
