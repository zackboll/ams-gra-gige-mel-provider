//===============================================================================
/// @file  SystemTrackDataResponse.h
/// @brief This file includes the SystemTrackDataResponse class definition.

#pragma once

#include <chrono>
#include <cstdint>

#include <math/geometry/RangeAzEl.h>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class SystemTrackDataResponse
	/// @brief Reports data of the response to the system track command.
	/// @Optional This class provides data definition in support of IR MEL functionality to report track information
	/// or enhance MFA OEM software related to pointing. While optional for IR MEL implementations, inclusion in
	/// a particular MFA instantiation indicates the importance of track data being provided to the MFA.  Failure to
	/// match support of this feature in a corresponding Service likely jeopardizes the ability of the MFA to provide
	/// high quality tracks and may have follow-on effects for pointing accuracy.
	class SystemTrackDataResponse
	{
	public:
		SystemTrackDataResponse() = default;
		~SystemTrackDataResponse() = default;
		SystemTrackDataResponse(const SystemTrackDataResponse&) = default;
		SystemTrackDataResponse(SystemTrackDataResponse&&) = default;
		SystemTrackDataResponse& operator=(const SystemTrackDataResponse&) = default;
		SystemTrackDataResponse& operator=(SystemTrackDataResponse&&) = default;

		[[nodiscard]] std::uint32_t getCommandID() const
		{
			return this->commandID;
		}
		void setCommandID(std::uint32_t commandID_in)
		{
			this->commandID = commandID_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getSystemTime() const
		{
			return this->systemTime;
		}
		void setSystemTime(std::chrono::nanoseconds systemTime_in)
		{
			this->systemTime = systemTime_in;
		}
		[[nodiscard]] std::uint32_t getRequestId() const
		{
			return this->requestId;
		}
		void setRequestId(std::uint32_t requestId_in)
		{
			this->requestId = requestId_in;
		}
		[[nodiscard]] std::uint32_t getTrackId() const
		{
			return this->trackId;
		}
		void setTrackId(std::uint32_t trackId_in)
		{
			this->trackId = trackId_in;
		}
		[[nodiscard]] double getRange() const
		{
			return this->range;
		}
		void setRange(double range_in)
		{
			this->range = range_in;
		}
		[[nodiscard]] double getRangeRate() const
		{
			return this->rangeRate;
		}
		void setRangeRate(double rangeRate_in)
		{
			this->rangeRate = rangeRate_in;
		}
		[[nodiscard]] double getRangeError() const
		{
			return this->rangeError;
		}
		void setRangeError(double rangeError_in)
		{
			this->rangeError = rangeError_in;
		}
		[[nodiscard]] double getRangeRateError() const
		{
			return this->rangeRateError;
		}
		void setRangeRateError(double rangeRateError_in)
		{
			this->rangeRateError = rangeRateError_in;
		}
		[[nodiscard]] bool getAzElValid() const
		{
			return this->AzElValid;
		}
		void setAzElValid(bool AzElValid_in)
		{
			this->AzElValid = AzElValid_in;
		}
		[[nodiscard]] AzEl getInertialAzEl() const
		{
			return this->inertialAzEl;
		}
		void setInertialAzEl(AzEl inertialAzEl_in)
		{
			this->inertialAzEl = inertialAzEl_in;
		}
		[[nodiscard]] AzEl getAzElError() const
		{
			return this->AzElError;
		}
		void setAzElError(AzEl AzElError_in)
		{
			this->AzElError = AzElError_in;
		}
		[[nodiscard]] bool getRangeValid() const
		{
			return this->rangeValid;
		}
		void setRangeValid(bool rangeValid_in)
		{
			this->rangeValid = rangeValid_in;
		}

	private:
		std::chrono::nanoseconds systemTime{0}; ///< in nanoseconds
		std::uint32_t commandID{0};
		std::uint32_t requestId{0};
		std::uint32_t trackId{0};
		double range{0.0};			///< in meters
		double rangeRate{0.0};		///< in meters / sec
		double rangeError{0.0};		///< in meters
		double rangeRateError{0.0}; ///< in meters / sec
		bool AzElValid{false};
		AzEl inertialAzEl{0.0, 0.0};
		AzEl AzElError{0.0, 0.0};
		bool rangeValid{false};
	};
} // end namespace ams::iface::irmel
