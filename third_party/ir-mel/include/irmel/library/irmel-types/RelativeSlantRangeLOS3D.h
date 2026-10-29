//===============================================================================
/// @file  RelativeSlantRangeLOS3D.h
/// @brief This file includes the RelativeSlantRangeLOS3D class
//===============================================================================
#pragma once

namespace ams::iface::irmel
{
	/// @class RelativeSlantRangeLOS3D
	/// @brief Contains relative LOS information
	/// @Required This class provides data definitions in support of required IR MEL functionality to support LOS information
	/// and must be included as-is in all IR MEL implementations
	class RelativeSlantRangeLOS3D
	{
	public:
		RelativeSlantRangeLOS3D() = default;
		RelativeSlantRangeLOS3D(double slantRange_in, double slantRangeError_in, double slantRangeRate_in, double slantRangeRateError_in)
			: slantRange{slantRange_in}, slantRangeError{slantRangeError_in}, slantRangeRate{slantRangeRate_in}, slantRangeRateError{slantRangeRateError_in}
		{
		}

		~RelativeSlantRangeLOS3D() = default;
		RelativeSlantRangeLOS3D(const RelativeSlantRangeLOS3D&) = default;
		RelativeSlantRangeLOS3D(RelativeSlantRangeLOS3D&&) = default;
		RelativeSlantRangeLOS3D& operator=(const RelativeSlantRangeLOS3D&) = default;
		RelativeSlantRangeLOS3D& operator=(RelativeSlantRangeLOS3D&&) = default;

		[[nodiscard]] double getSlantRange() const
		{
			return this->slantRange;
		}
		void setSlantRange(double slantRange_in)
		{
			this->slantRange = slantRange_in;
		}

		[[nodiscard]] double getSlantRangeError() const
		{
			return this->slantRangeError;
		}
		void setSlantRangeError(double slantRangeError_in)
		{
			this->slantRangeError = slantRangeError_in;
		}

		[[nodiscard]] double getSlantRangeRate() const
		{
			return this->slantRangeRate;
		}
		void setSlantRangeRate(double slantRangeRate_in)
		{
			this->slantRangeRate = slantRangeRate_in;
		}

		[[nodiscard]] double getSlantRangeRateError() const
		{
			return this->slantRangeRateError;
		}
		void setSlantRangeRateError(double slantRangeRateError_in)
		{
			this->slantRangeRateError = slantRangeRateError_in;
		}

	private:
		// Indicates the line-of-sight distance between two points.
		double slantRange{0};
		// Discrete value indicating the error in SlantRange.
		double slantRangeError{0};
		// Indicates the relative velocity along the line-of-sight.
		double slantRangeRate{0};
		// Discrete value indicating the one sigma error in SlantRangeRate.
		double slantRangeRateError{0};
	};
} // namespace ams::iface::irmel
