//===============================================================================
/// @file  StareTaskParam.h
/// @brief This file includes the parameter required to command a stare task.

#pragma once

#include <chrono>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <math/geometry/RangeAzEl.h>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class StareTaskParam
	/// @brief Parameters needed to command a Stare task
	/// @Required This class provides data definition in support of IR MEL functionality to report stare task parameters
	/// and must be included as-is in all IR MEL implementations
	class StareTaskParam
	{
	public:
		StareTaskParam() = default;
		~StareTaskParam() = default;
		StareTaskParam(const StareTaskParam&) = default;
		StareTaskParam(StareTaskParam&&) = default;
		StareTaskParam& operator=(const StareTaskParam&) = default;
		StareTaskParam& operator=(StareTaskParam&&) = default;

		[[nodiscard]] AzEl getCenter() const
		{
			return this->center;
		}
		void setCenter(AzEl center_in)
		{
			this->center = center_in;
		}
		[[nodiscard]] CoordFrameRef getCentFrameRefEl() const
		{
			return this->centFrameRefEl;
		}
		void setCentFrameRefEl(CoordFrameRef centFrameRefEl_in)
		{
			this->centFrameRefEl = centFrameRefEl_in;
		}
		[[nodiscard]] CoordFrameRef getCentFrameRefAz() const
		{
			return this->centFrameRefAz;
		}
		void setCentFrameRefAz(CoordFrameRef centFrameRefAz_in)
		{
			this->centFrameRefAz = centFrameRefAz_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getDurationNanoseconds() const
		{
			return this->durationNanoseconds;
		}
		void setDurationNanoseconds(std::chrono::nanoseconds durationNanoseconds_in)
		{
			this->durationNanoseconds = durationNanoseconds_in;
		}

	private:
		AzEl center;				  ///< inertial, with platform as origin
		CoordFrameRef centFrameRefEl{
			CoordFrameRef::Inertial}; ///< Specifies whether "elevation center" is expressed as an Inertial or Aircraft frame of reference
		CoordFrameRef centFrameRefAz{
			CoordFrameRef::Aircraft}; ///< Specifies whether "azimuth center" is expressed as an Inertial or Aircraft frame of reference
		std::chrono::nanoseconds durationNanoseconds{0};
	};
} // namespace ams::iface::irmel
