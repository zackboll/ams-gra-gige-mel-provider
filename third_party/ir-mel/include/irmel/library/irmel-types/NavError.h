//===============================================================================
/// @file  NavError.h
/// @brief This file includes the NavError class

#pragma once

namespace ams::iface::irmel
{
	/// @class NavError
	/// @brief Reports error in 3-4 dimensions.
	/// Intended for position and orientation error data in vector, Euler, or Quaternion format.
	/// @Optional This class provides data definition in support of IR MEL functionality to report internal navigation state of the sensor at the time
	/// of frame collection.
	class NavError
	{
	public:
		NavError() = default;
		NavError(double x_in, double y_in, double z_in) : x{x_in}, y{y_in}, z{z_in}
		{
		}
		NavError(double x_in, double y_in, double z_in, double w_in) : x{x_in}, y{y_in}, z{z_in}, w{w_in}
		{
		}
		~NavError() = default;
		NavError(const NavError&) = default;
		NavError(NavError&&) = default;
		NavError& operator=(const NavError&) = default;
		NavError& operator=(NavError&&) = default;

		// Getters/Setters
		[[nodiscard]] double getX() const
		{
			return this->x;
		}
		void setX(double x_in)
		{
			this->x = x_in;
		}
		[[nodiscard]] double getY() const
		{
			return this->y;
		}
		void setY(double y_in)
		{
			this->y = y_in;
		}
		[[nodiscard]] double getZ() const
		{
			return this->z;
		}
		void setZ(double z_in)
		{
			this->z = z_in;
		}
		[[nodiscard]] double getW() const
		{
			return this->w;
		}
		void setW(double w_in)
		{
			this->w = w_in;
		}

	private:
		double x{0};
		double y{0};
		double z{0};
		double w{0}; // w is used only for Quaternion error
	};

} // namespace ams::iface::irmel
