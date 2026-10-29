//===============================================================================
/// @file  LOS3D_OrientationCovariance.h
/// @brief This file includes the LOS3D_OrientationCovariance class
//===============================================================================
#pragma once

namespace ams::iface::irmel
{
	/// @class LOS3D_OrientationCovariance
	/// @brief Line of sight orientation covariance definitions
	/// @Required This class provides data definitions in support or required IR MEL functionality to report Line of sight orientation
	/// covariance and must be included as-is in all IR MEL implementations
	class LOS3D_OrientationCovariance
	{
	public:
		LOS3D_OrientationCovariance() = default;
		LOS3D_OrientationCovariance(double rvRv_in, double rvRh_in, double rhRh_in, double psRv_in, double psRh_in, double psPs_in)
			: rvRv{rvRv_in}, rvRh{rvRh_in}, rhRh{rhRh_in}, psRv{psRv_in}, psRh{psRh_in}, psPs{psPs_in}
		{
		}

		~LOS3D_OrientationCovariance() = default;
		LOS3D_OrientationCovariance(const LOS3D_OrientationCovariance&) = default;
		LOS3D_OrientationCovariance(LOS3D_OrientationCovariance&&) = default;
		LOS3D_OrientationCovariance& operator=(const LOS3D_OrientationCovariance&) = default;
		LOS3D_OrientationCovariance& operator=(LOS3D_OrientationCovariance&&) = default;

		[[nodiscard]] double getRvRv() const
		{
			return this->rvRv;
		}
		void setRvRv(double rvRv_in)
		{
			this->rvRv = rvRv_in;
		}
		[[nodiscard]] double getRvRh() const
		{
			return this->rvRh;
		}
		void setRvRh(double rvRh_in)
		{
			this->rvRh = rvRh_in;
		}

		[[nodiscard]] double getRhRh() const
		{
			return this->rhRh;
		}
		void setRhRh(double rhRh_in)
		{
			this->rhRh = rhRh_in;
		}

		[[nodiscard]] double getPsRv() const
		{
			return this->psRv;
		}
		void setPsRv(double psRv_in)
		{
			this->psRv = psRv_in;
		}

		[[nodiscard]] double getPsRh() const
		{
			return this->psRh;
		}
		void setPsRh(double psRh_in)
		{
			this->psRh = psRh_in;
		}

		[[nodiscard]] double getPsPs() const
		{
			return this->psPs;
		}
		void setPsPs(double psPs_in)
		{
			this->psPs = psPs_in;
		}

	private:
		// Row 1 Column 1 term of the result matrix. Vertical Vertical angle-angle covariance.
		// As radian is unitless result is unitless.
		double rvRv{0};
		// Row 1 Column 2 term of the result matrix. Vertical Horizontal angle-angle covariance.
		// As radian is unitless result is unitless.
		double rvRh{0};
		// Row 2 Column 2 term of the result matrix. Horizontal Horizontal angle-angle covariance.
		// As radian is unitless result is unitless.
		double rhRh{0};
		// Row 1 Column 3 term of the result matrix. Slant Range Vertical position-angle covariance.
		// As radian is unitless result is unitless.
		double psRv{0};
		// Row 2 Column 3 term of the result matrix. Slant Range Horizontal position-angle covariance.
		// As radian is unitless result is unitless.
		double psRh{0};
		// Row 3 Column 3 term of the result matrix. Slant Range Slant Range position-position covariance.
		// As radian is unitless result is unitless.
		double psPs{0};
	};
} // namespace ams::iface::irmel
