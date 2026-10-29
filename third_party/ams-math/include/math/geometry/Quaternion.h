/// \file Quaternion.h
/// \brief Contains templated Quaternion class definition and implementation

#pragma once

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

using std::fabs;
using std::sqrt;

namespace ams::util::math::geometry
{
	template <typename T>
	using vec3 = std::array<T, 3>;

	// clang-format off

	/**
	 * @class This class describes a Quaternion (W,X,Y,Z), which is a way to describe the orientation of an object.
	 * Unlike Euler angles, which use 3 numbers, this uses 4 numbers. In doing so, they avoid the common problem of
	 * "Gimbal Lock" that occurs when strictly using Euler angles, when approaching a singularity (north or south pole).
	 * Because of this, Quaternions should be preferred over Euler angles for holding orientation state information
	 * in a dynamic context. (i.e. storing current platform orientation, etc).
	 *
	 * The conversion functions in this class assume the eulers are STORED in vectors as:
	 *
	 * [X, Y, Z] = [ Roll, Yaw, Pitch ]
	 *
	 * For manually checking results, the Euler rotation sequence order is:
	 *
	 * 1.) Yaw   - Rotate CCW around +Y
	 *
	 * 2.) Pitch - Rotate CW around +Z
	 *
	 * 3.) Roll  - Rotate CW around +X
	 *
	 * @note All operations inherit whatever the reference cartesian XYZ frame implies.
	 * All length measurements are unitless unless otherwise specified.
	 * All angular units are in radians unless otherwise specified.
	 *
	 * @tparam T float, double, long double
	 */
	/// \class Quaternion
	/// \brief This class describes a Quaternion (W,X,Y,Z), which is a way to describe the orientation of an object.
	template <typename T>
	class Quaternion
	{
	public:
		// default to a unrotated normalized quaternion (1, 0, 0, 0)
		constexpr Quaternion() : e{static_cast<T>(1.0), static_cast<T>(0.0), static_cast<T>(0.0), static_cast<T>(0.0)}
		{
		}

		constexpr Quaternion(T w, T x, T y, T z) : e{w, x, y, z}
		{
			this->normalize();
		}

		/// @brief Construct a new Quaternion object given euler angles in radians
		/// @param roll X-axis rotation (bank)
		/// @param yaw Y-axis rotation (heading)
		/// @param pitch Z-axis rotation (attitude)
		constexpr Quaternion(T roll, T yaw, T pitch)
		{
			fromEulerAngles(vec3<T>{roll, yaw, pitch});
		}

		/// @brief Construct a new Quaternion object given euler angles
		/// in vector (vec3) form.
		/// @param ryp A Roll-Yaw-Pitch (XYZ) euler vector in radians
		constexpr explicit Quaternion(const vec3<T> &ryp) { fromEulerAngles(ryp); }

		/// @brief Indexed accessors
		constexpr T& operator[](size_t idx) noexcept { return this->e[idx]; }
		constexpr const T& operator[](size_t idx) const noexcept { return this->e[idx]; }

		/// @brief Accessors for W, the first element
		constexpr T w() const noexcept { return this->e[0]; }
		constexpr T& w() noexcept { return this->e[0]; }
		constexpr void w(const T& val) noexcept { this->e[0] = val; }

		/// @brief Accessors for X, the second element
		constexpr T x() const noexcept { return this->e[1]; }
		constexpr T& x() noexcept { return this->e[1]; }
		constexpr void x(const T& val) noexcept { this->e[1] = val; }

		/// @brief Accessors for Y, the third element
		constexpr T y() const noexcept { return this->e[2]; }
		constexpr T& y() noexcept { return this->e[2]; }
		constexpr void y(const T& val) noexcept { this->e[2] = val; }

		/// @brief Accessors for Z, the fourth element
		constexpr T z() const noexcept { return this->e[3]; }
		constexpr T& z() noexcept { return this->e[3]; }
		constexpr void z(const T& val) noexcept { this->e[3] = val; }

		/// @brief native conversion to std::array
		constexpr explicit operator std::array<T, 4>() const noexcept { return this->e; }

		/// @brief native conversion to std::vector
		constexpr explicit operator std::vector<T>() const noexcept
		{
			return std::vector<T>{w(), x(), y(), z()};
		}

		constexpr T operator[](int i) const
		{
			return e.at(i);
		}

		constexpr T &operator[](int i)
		{
			return e.at(i);
		}

		/**
		 * @brief Scalar multiplication-assignment operator
		 *
		 * @param t scalar to multiply by
		 * @return Quaternion&
		 */
		constexpr Quaternion &operator*=(const T &t)
		{
			const T tw = w() * t;
			const T tx = x() * t;
			const T ty = y() * t;
			const T tz = z() * t;

			w(tw);
			x(tx);
			y(ty);
			z(tz);

			return *this;
		}

		/**
		 * @brief Multiply this quaternion by another quaternion "v"
		 * This essentially rotates this quaternion by the
		 * given quaternions amount. (u * v)
		 *
		 * @note Quaternion multiplication is NON-COMMUTATIVE
		 *
		 * @param v the quaternion to multiply by
		 * @return Quaternion&
		 */
		constexpr Quaternion &operator*=(const Quaternion &v)
		{
			const T uw = w();
			const T ux = x();
			const T uy = y();
			const T uz = z();

			const T vw = v.w();
			const T vx = v.x();
			const T vy = v.y();
			const T vz = v.z();

			w(uw * vw - ux * vx - uy * vy - uz * vz);
			x(uw * vx + ux * vw + uy * vz - uz * vy);
			y(uw * vy - ux * vz + uy * vw + uz * vx);
			z(uw * vz + ux * vy - uy * vx + uz * vw);

			return *this;
		}

		/**
		 * @brief  Computes the conjugate of this quaternion.
		 * a.k.a (w, -x, -y, -z)
		 *
		 * @return Quaternion conjugate
		 */
		Quaternion conjugate() const
		{
			Quaternion q = *this;
			// leave w alone
			q.x(-q.x());
			q.y(-q.y());
			q.z(-q.z());
			return q;
		}

		/**
		 * @brief The equivalent quaternion, but with signs of all the components flipped.
		 * ex. If this was (-W, +X, +Y, -Z), this will return (+W, -X, -Y, +Z)
		 *
		 * @return constexpr Quaternion with flipped signs
		 */
		constexpr Quaternion twin() const
		{
			Quaternion q = conjugate();
			q.w(-this->w());
			return q;
		}

		constexpr T length_squared() const
		{
			return (w() * w()) +  (x() * x()) + (y() * y()) +  (z() * z());
		}

		constexpr T length() const { return std::sqrt(length_squared()); }

		constexpr T mag() const { return length(); }

		/// @brief Scales everything back to a unit quaternion
		void normalize()
		{
			constexpr T ONE = T(1.0);
			const T invmag = mag() != 0 ? ONE / mag() : T(0.0);
			w(w() * invmag);
			x(x() * invmag);
			y(y() * invmag);
			z(z() * invmag);
		}

		/**
		 * @brief Ensures that the real part of the quaternion (w) is always positive.
		 * replaces the current values with the twinned if necessary.
		 */
		constexpr void standardize()
		{
			if (std::signbit(w())) { *this = twin(); }
		}

		/**
		 * @brief Perform the dot product between this Quaternion
		 * and another.
		 *
		 * @param v other quaternion
		 * @return dot product result.
		 */
		T dot(const Quaternion &v) const
		{
			const T uw = w();
			const T ux = x();
			const T uy = y();
			const T uz = z();

			const T vw = v.w();
			const T vx = v.x();
			const T vy = v.y();
			const T vz = v.z();

			return uw * vw + ux * vx + uy * vy + uz * vz;
		}

		/**
		 * @brief Returns the total angular difference between this and
		 * another quaternion.
		 *
		 * @param v The other quaternion
		 * @return T Angle in radians
		 */
		T angle(const Quaternion &v) const
		{
			constexpr T TWO = static_cast<T>(2.0);
			const Quaternion uconj = this->conjugate();
			const Quaternion vconj = v.conjugate();
			const Quaternion prodUconjV = uconj * v;	   // NOT EQUAL TO v * uconj
			const Quaternion prodVconjU = vconj * (*this); // NOT EQUAL TO u * vconj
			const T dotProd = prodUconjV.dot(prodVconjU);
			return std::acos(dotProd) * TWO;
		}

		/**
		 * @brief Assumes VRML reference frame using radians:
		 * (underscores are just for vscode spacing)
		 *
		 * < euler[0]  |  psi__  |  CW around +X (forward) |  roll__ |  bank >
		 * < euler[1]  |  theta  |  CCW around +Y (up)____ |  yaw_   |  heading >
		 * < euler[2]  |  phi__  |  CW around +Z (right)__ |  pitch  |  attidude >
		 *
		 * @param euler <X,Y,Z> vector containing euler angles in radians:
		 *  <psi, theta, phi> a.k.a <roll, yaw, pitch>
		 */
		void fromEulerAngles(const vec3<T> &euler)
		{
			const T psi = euler[0];	  // CW +X (forward), roll,  bank
			const T theta = euler[1]; // CW +Y (up)   ,   yaw,   heading
			const T phi = euler[2];	  // CW +Z (right),   pitch, attidude

			constexpr T half = static_cast<T>(0.5);
			const T cr = std::cos(phi * half);
			const T sr = std::sin(phi * half);
			const T cp = std::cos(theta * half);
			const T sp = std::sin(theta * half);
			const T cy = std::cos(psi * half);
			const T sy = std::sin(psi * half);

			w(cr * cp * cy + sr * sp * sy);
			x(sr * cp * cy - cr * sp * sy);
			y(cr * sp * cy + sr * cp * sy);
			z(cr * cp * sy - sr * sp * cy);

			this->normalize();
		}

		/**
		 * @brief Assumes the follow geometric mapping:
		 * (underscores are just for vscode spacing)
		 *
		 * < euler[0]  |  psi__  |  CW around +X (forward) |  roll__  |  bank >
		 * < euler[1]  |  theta  |  CW around +Y (up)____  |  yaw_    |  heading >
		 * < euler[2]  |  phi__  |  CW around +Z (right)__ |  pitch  |  attidude >
		 *
		 *
		 * @return vec3<T> <X,Y,Z> vector containing euler angles in radians:
		 *  <psi, theta, phi> a.k.a <roll, yaw, pitch>
		 */
		vec3<T> toEulerAngles() const
		{
			vec3<T> rpy;

			Quaternion<T> quat = *this;
			quat.normalize();

			const T qw = quat.w();
			const T qx = quat.x();
			const T qy = quat.y();
			const T qz = quat.z();

			constexpr T one = static_cast<T>(1.0);
			constexpr T two = static_cast<T>(2.0);

			// x-axis rotation
			const T sinr_cosp = two * (qw * qx + qy * qz);
			const T cosr_cosp = one - two * (qx * qx + qy * qy);
			rpy[0] = std::atan2(sinr_cosp, cosr_cosp);

			// y-axis rotation
			const T sinp = std::sqrt(one + two * (qw * qy - qx * qz));
			const T cosp = std::sqrt(one - two * (qw * qy - qx * qz));
			rpy[1] = two * std::atan2(sinp, cosp) - M_PI / two;

			// z-axis rotation
			const T siny_cosp = two * (qw * qz + qx * qy);
			const T cosy_cosp = one - two * (qy * qy + qz * qz);
			rpy[2] = std::atan2(siny_cosp, cosy_cosp);

			return rpy;
		}

		/**
		 * @brief Return the <+X, +Y, +Z> unit direction vector of where
		 * this quaternion is pointing, assuming they are using the same reference frame.
		 *
		 * @return vec3<T> The <+X, +Y, +Z> unit direction vector.
		 */
		vec3<T> toUnitDirectionVector() const
		{
			constexpr T TOLERANCE = T(1e-8);

			constexpr vec3<T> DEFAULT{T(1.0), T(), T()};

			Quaternion q{*this};
			q.standardize();

			// check if the all the imaginary components are zero, which would actually mean it was default unit quaternion
			if((std::abs(q.x()) < TOLERANCE) && (std::abs(q.y()) < TOLERANCE) && (std::abs(q.z()) < TOLERANCE))
			{
				return DEFAULT;
			}

			const T imaglen = std::sqrt(q.x() * q.x() + q.y() * q.y() + q.z() * q.z());

			return vec3<T>{q.x() / imaglen, q.y() / imaglen, q.z() / imaglen};
		}

	private:
		std::array<T, 4> e; // W = e[0], X = e[1], Y = e[2], Z = e[3]
	};

	// clang-format on

	// third person multiplication operator
	template <typename T>
	inline Quaternion<T> operator*(const Quaternion<T> &lhs, const Quaternion<T> &rhs)
	{
		Quaternion<T> q = lhs;
		q *= rhs;
		return q;
	}

	// third person scalar multiplication operator
	template <typename T>
	inline Quaternion<T> operator*(const T &lhs, const Quaternion<T> &rhs)
	{
		Quaternion<T> q = rhs;
		q *= lhs;
		return q;
	}

	// third person scalar multiplication operator
	template <typename T>
	inline Quaternion<T> operator*(const Quaternion<T> &lhs, const T &rhs)
	{
		Quaternion<T> q = lhs;
		q *= rhs;
		return q;
	}
} // namespace ams::util::math::geometry
