//===============================================================================
/// @file  IR_Directional.h
/// @brief This file includes directional types.

#pragma once

#include <mel/library/Directional.h>

namespace ams::iface::irmel
{
	/// @class IR_Directional
	/// @brief Indicates the location of the vector
	/// @Required This class provides data definition in support of required IR MEL functionality to interpret the
	/// direction of an IR MEL and must be included as-is in all IR MEL implementations
	class IR_Directional : protected ams::iface::mel::BaseDirectional
	{
	public:
		IR_Directional() = default;
		IR_Directional(double xx, double yy, double zz) : BaseDirectional(xx, yy, zz)
		{
		}
		~IR_Directional() = default;
		IR_Directional(const IR_Directional&) = default;
		IR_Directional(IR_Directional&&) = default;
		IR_Directional& operator=(const IR_Directional&) = default;
		IR_Directional& operator=(IR_Directional&&) = default;

		void SetAllAxis(double x, double y, double z)
		{
			this->xAxis = x;
			this->yAxis = y;
			this->zAxis = z;
		}

		[[nodiscard]] double getX() const
		{
			return this->xAxis;
		}
		void setX(double x)
		{
			this->xAxis = x;
		}
		[[nodiscard]] double getY() const
		{
			return this->yAxis;
		}
		void setY(double y)
		{
			this->yAxis = y;
		}
		[[nodiscard]] double getZ() const
		{
			return this->zAxis;
		}
		void setZ(double z)
		{
			this->zAxis = z;
		}
	};

	/// @class ForwardRightDown
	/// @brief Coordinate Frame used in OpticalDistortionMap to find direction ForwardRightDown
	/// @Required This class provides data definition in support of required IR MEL functionality to identify axis direction
	/// and must be included as-is in all IR MEL implementations
	class ForwardRightDown : protected ams::iface::mel::BaseDirectional
	{
	public:
		ForwardRightDown() = default;
		ForwardRightDown(double f, double r, double d) : BaseDirectional(f, r, d)
		{
		}
		~ForwardRightDown() = default;
		ForwardRightDown(const ForwardRightDown&) = default;
		ForwardRightDown(ForwardRightDown&&) = default;
		ForwardRightDown& operator=(const ForwardRightDown&) = default;
		ForwardRightDown& operator=(ForwardRightDown&&) = default;

		void SetAllAxis(double forward, double right, double down)
		{
			this->xAxis = forward;
			this->yAxis = right;
			this->zAxis = down;
		}

		[[nodiscard]] double getForward() const
		{
			return this->xAxis;
		}
		void setForward(double forward)
		{
			this->xAxis = forward;
		}
		[[nodiscard]] double getRight() const
		{
			return this->yAxis;
		}
		void setRight(double right)
		{
			this->yAxis = right;
		}
		[[nodiscard]] double getDown() const
		{
			return this->zAxis;
		}
		void setDown(double down)
		{
			this->zAxis = down;
		}
	};

	/// @class RowCol
	/// @brief Creates the number of rows and columns to hold the data implemented from BaseDirectional().
	/// Class used in OpticalDistortionMap
	/// @Required This class provides data definition in support of IR MEL functionality to indicate rows and columns
	/// and must be included as-is in all IR MEL implementations
	class RowCol : protected ams::iface::mel::BaseDirectional
	{
	public:
		RowCol() = default;
		RowCol(double r, double c) : BaseDirectional(r, c, 0)
		{
		}
		~RowCol() = default;
		RowCol(const RowCol&) = default;
		RowCol(RowCol&&) = default;
		RowCol& operator=(const RowCol&) = default;
		RowCol& operator=(RowCol&&) = default;

		void SetAllAxis(double row, double col)
		{
			this->xAxis = row;
			this->yAxis = col;
		}

		[[nodiscard]] double getRow() const
		{
			return this->xAxis;
		}
		void setRow(double row)
		{
			this->xAxis = row;
		}
		[[nodiscard]] double getCol() const
		{
			return this->yAxis;
		}
		void setCol(double col)
		{
			this->yAxis = col;
		}
	};

	/// @class Quaternion
	/// @brief Calculates the Quaternion of an axis in a direction
	/// @Required This class provides data definition in support of required IR MEL functionality to indicate quaternion units
	/// and must be included as-is in all IR MEL implementations
	class Quaternion : protected ams::iface::mel::BaseDirectional
	{
	public:
		Quaternion() = default;
		Quaternion(double xx, double yy, double zz, double ww) : ams::iface::mel::BaseDirectional(xx, yy, zz), wQuaternionAxis{ww}
		{
		}
		~Quaternion() = default;
		Quaternion(const Quaternion&) = default;
		Quaternion(Quaternion&&) = default;
		Quaternion& operator=(const Quaternion&) = default;
		Quaternion& operator=(Quaternion&&) = default;

		void SetAllQuaternionAxis(double xx, double yy, double zz, double ww)
		{
			this->xAxis = xx;
			this->yAxis = yy;
			this->zAxis = zz;
			this->wQuaternionAxis = ww;
		}

		[[nodiscard]] double getQuaternionX() const
		{
			return this->xAxis;
		}
		void setQuaternionX(double x)
		{
			this->xAxis = x;
		}
		[[nodiscard]] double getQuaternionY() const
		{
			return this->yAxis;
		}
		void setQuaternionY(double y)
		{
			this->yAxis = y;
		}
		[[nodiscard]] double getQuaternionZ() const
		{
			return this->zAxis;
		}
		void setQuaternionZ(double z)
		{
			this->zAxis = z;
		}
		[[nodiscard]] double getQuaternionW() const
		{
			return this->wQuaternionAxis;
		}
		void setQuaternionW(double w)
		{
			this->wQuaternionAxis = w;
		}

	private:
		double wQuaternionAxis{0};
	};
} // end namespace  ams::iface::irmel
