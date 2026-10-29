//===============================================================================
/// @file  TempPoint.h
/// @brief This file includes the TempPoint type.
//===============================================================================
#pragma once

#include <chrono>
#include <optional>
#include <variant>

#include <irmel/library/irmel-types/BandInfo.h>
#include <cstdint>

namespace ams::iface::irmel
{
	/// @enum TempPointType
	/// @brief Used to describe what type of temperature point is being provided
	/// @Optional For MFAs that perform NUC, this class represents the source of a TempPoint
	enum class TempPointType : uint8_t
	{
		NUC_Point,	  ///< A NUC point in the MFA's NUC process
		MFA_Estimate, ///< This point is either interpolated or extrapolated based on supplier data and/or existing NUC_Points
		Other		  ///< Indicates an invalid or unsubstantiated point
	};

	/// @class TempPoint
	/// @brief Provides a timestamped point which correlates to a pixel value in a frame to the predicted temperature of that pixel
	/// @Optional For MFAs that perform NUC, this class a data object to represent NUC data
	class TempPoint
	{
	public:
		/// @brief The point's pixel value. Use the integer type appropriate for your MFA's image
		/// @Optional This type can be optionally used to describe the point's pixel value.
		using PixelValue = std::variant<std::uint8_t, std::uint16_t, std::uint32_t>;
		TempPoint() = default;
		TempPoint(std::chrono::nanoseconds timestamp_in, BandInfo frameBand_in, PixelValue pixelValue_in, double temperature_in, std::optional<double> tempError_in,
				  TempPointType pointType_in)
			: timestamp{timestamp_in}, frameBand{frameBand_in}, pixelValue{pixelValue_in}, temperature{temperature_in}, tempError{tempError_in}, pointType{pointType_in}
		{
		}
		~TempPoint() = default;
		TempPoint(const TempPoint&) = default;
		TempPoint(TempPoint&&) = default;
		TempPoint& operator=(const TempPoint&) = default;
		TempPoint& operator=(TempPoint&&) = default;

		[[nodiscard]] std::chrono::nanoseconds getTimestamp() const
		{
			return this->timestamp;
		}
		void setTimestamp(std::chrono::nanoseconds timestamp_in)
		{
			this->timestamp = timestamp_in;
		}
		[[nodiscard]] BandInfo getFrameBand() const
		{
			return this->frameBand;
		}
		void setFrameBand(const BandInfo& frameBand_in)
		{
			this->frameBand = frameBand_in;
		}
		[[nodiscard]] const PixelValue& getPixelValue() const
		{
			return this->pixelValue;
		}
		void setPixelValue(const PixelValue& pixelValue_in)
		{
			this->pixelValue = pixelValue_in;
		}
		[[nodiscard]] double getTemperature() const
		{
			return this->temperature;
		}
		void setTemperature(double temperature_in)
		{
			this->temperature = temperature_in;
		}
		[[nodiscard]] TempPointType getPointType() const
		{
			return this->pointType;
		}
		void setPointType(TempPointType pointType_in)
		{
			this->pointType = pointType_in;
		}

	private:
		/// The time the point was last updated. Timestamping done on a per-point basis
		/// to enable compatibility with MFAs that take NUC points asynchronously
		std::chrono::nanoseconds timestamp{0};
		BandInfo frameBand{};  ///< Provides information on which band this Temp Point applies to.
		PixelValue pixelValue; ///< The point's pixel value. Use the integer type appropriate for your MFA's image
		double temperature{0}; ///< Temperature in Kelvin of this point's pixelValue
		/// 1-sigma error on the Temperature of this point. This field is optional to allow for
		/// MFAs which do not know or compute this information. The error may be measured directly
		/// during the NUC process, estimated based on analysis performed and/or lab data collected
		/// by the MFA provider
		std::optional<double> tempError;
		TempPointType pointType{TempPointType::Other}; ///< Enumeration describes which type of point the TempPoint object is
	};
} // end namespace ams::iface::irmel
