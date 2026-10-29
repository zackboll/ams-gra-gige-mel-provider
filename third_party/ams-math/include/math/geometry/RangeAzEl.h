#pragma once

#include <cstddef>
#include <stdexcept>

/** @struct AzEl
 *  Struct for storing angle data
 *
 *  @var AzEl::az
 *    Azimuth component of angle in rad
 *  @var AzEl::el
 *    Elevation component of angle in rad
 */
struct AzEl
{
	double az = 0.0;
	double el = 0.0;

	enum Index
	{
		Az = 0,
		El,
		N
	};

	/**
	 * @brief operator[]
	 *
	 * accessor function
	 */
	double& operator[](size_t i)
	{
		if (i >= N) throw std::out_of_range("Index out of bounds");
		double* pArray = &az;
		return pArray[i];
	}

	/**
	 * @brief operator[]
	 *
	 * accessor function
	 */
	const double& operator[](size_t i) const
	{
		if (i >= N) throw std::out_of_range("Index out of bounds");
		const double* pArray = &az;
		return pArray[i];
	}
};

#define FOREACH_AE(idx) for(size_t idx = 0; (idx) < AzEl::N; (idx)++)

/** @struct RangeAzEl
 *  Struct for storing Range, Range Rate, and angle data
 *  @var RangeAzEl::range
 *    Range in meters
 *  @var RangeAzEl::az
 *    Azimuth component of angle in rad
 *  @var RangeAzEl::el
 *    Elevation component of angle in rad
 *  @var RangeAzEl::rangerate
 *    Range-rate in meters/sec
 */
struct RangeAzEl
{
	double range = 0.0;
	double az = 0.0;
	double el = 0.0;
	double rangerate = 0.0;

	enum Index
	{
		Range = 0,
		Az,
		El,
		RangeRate,
		N
	};

	/**
	 * @brief operator[]
	 *
	 * accessor function
	 */
	double& operator[](size_t i)
	{
		if (i >= N) throw std::out_of_range("Index out of bounds");
		double* pArray = &range;
		return pArray[i];
	}

	/**
	 * @brief operator[]
	 *
	 * accessor function
	 */
	const double& operator[](size_t i) const
	{
		if (i >= N) throw std::out_of_range("Index out of bounds");
		const double* pArray = &range;
		return pArray[i];
	}
};
#define FOREACH_RAE(idx) for(size_t idx = 0; (idx) < RangeAzEl::N; (idx)++)
