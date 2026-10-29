//===============================================================================
/// @file  Uncertainty.h
/// @brief This file includes the Uncertainty class

#pragma once
#include <cstdint>

namespace ams::iface::irmel
{
	/// @class Uncertainty
	/// @brief Reports an uncertainities brought on by the sensor or platform
	/// @Required This class provides data definition in support of required IR MEL functionality to report uncertainty
	/// and must be included as-is in all IR MEL implementations
	class Uncertainty
	{
	public:
		Uncertainty() = default;
		Uncertainty(std::uint32_t sensor, std::uint32_t plat) : sensorUncertainties{sensor}, platformUncertainties{plat}
		{
		}
		~Uncertainty() = default;
		Uncertainty(const Uncertainty&) = default;
		Uncertainty(Uncertainty&&) = default;
		Uncertainty& operator=(const Uncertainty&) = default;
		Uncertainty& operator=(Uncertainty&&) = default;

		[[nodiscard]] std::uint32_t getSensorUncertainties() const
		{
			return this->sensorUncertainties;
		}
		void setSensorUncertainties(std::uint32_t sensorUncertainties_in)
		{
			this->sensorUncertainties = sensorUncertainties_in;
		}
		[[nodiscard]] std::uint32_t getPlatformUncertainties() const
		{
			return this->platformUncertainties;
		}
		void setPlatformUncertainties(std::uint32_t platformUncertainties_in)
		{
			this->platformUncertainties = platformUncertainties_in;
		}

	private:
		std::uint32_t sensorUncertainties{0};
		std::uint32_t platformUncertainties{0};
	};
} // namespace ams::iface::irmel
