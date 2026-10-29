//===============================================================================
/// @file  CandidateObjectHeader.h
/// @brief This file includes the data definition of the candidate object header, used
/// by the candidate object message.

#pragma once

#include <chrono>
#include <vector>
#include <utility>

#include <irmel/library/irmel-types/HotRegion.h>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class CandidateObjectHeader
	/// @brief Information about the header used in the candidate object
	/// @RequiredIfTrack This class provides data definition in support of conditionally required IR MEL functionality to report candidate objects
	/// and must be included as-is in all IR MEL implementations that support Tracks.
	class CandidateObjectHeader
	{
	public:
		CandidateObjectHeader() = default;
		CandidateObjectHeader(std::uint16_t num, std::chrono::nanoseconds tov) : numberOfCOs{num}, TOVutcNanoseconds{tov}
		{
		}
		CandidateObjectHeader(std::uint16_t num, std::uint16_t index, float CFAR_in, std::uint16_t field, std::chrono::nanoseconds tov,
							  std::vector<HotRegion> regions)
			: numberOfCOs{num},
			  stackFrameIndex{index},
			  CFAR{CFAR_in},
			  validityFlagBitField{field},
			  TOVutcNanoseconds{tov},
			  hotRegions{std::move(regions)}
		{
		}
		~CandidateObjectHeader() = default;
		CandidateObjectHeader(const CandidateObjectHeader&) = default;
		CandidateObjectHeader(CandidateObjectHeader&&) = default;
		CandidateObjectHeader& operator=(const CandidateObjectHeader&) = default;
		CandidateObjectHeader& operator=(CandidateObjectHeader&&) = default;

		[[nodiscard]] std::uint16_t getNumberOfCOs() const
		{
			return this->numberOfCOs;
		}
		void setNumberOfCOs(std::uint16_t numberOfCOs_in)
		{
			this->numberOfCOs = numberOfCOs_in;
		}
		[[nodiscard]] std::uint16_t getStackFrameIndex() const
		{
			return this->stackFrameIndex;
		}
		void setStackFrameIndex(std::uint16_t stackFrameIndex_in)
		{
			this->stackFrameIndex = stackFrameIndex_in;
		}
		[[nodiscard]] float getCFAR() const
		{
			return this->CFAR;
		}
		void setCFAR(float CFAR_in)
		{
			this->CFAR = CFAR_in;
		}
		[[nodiscard]] std::uint16_t getValidityFlagBitField() const
		{
			return this->validityFlagBitField;
		}
		void setValidityFlagBitField(std::uint16_t validityFlagBitField_in)
		{
			this->validityFlagBitField = validityFlagBitField_in;
		}
		[[nodiscard]] std::chrono::nanoseconds getTOVutcNanoseconds() const
		{
			return this->TOVutcNanoseconds;
		}
		void setTOVutcNanoseconds(std::chrono::nanoseconds TOVutcNanoseconds_in)
		{
			this->TOVutcNanoseconds = TOVutcNanoseconds_in;
		}
		[[nodiscard]] const std::vector<HotRegion>& getHotRegions() const
		{
			return this->hotRegions;
		}
		void setHotRegions(const std::vector<HotRegion>& regions)
		{
			this->hotRegions = regions;
		}
		void addHotRegion(const HotRegion& region)
		{
			this->hotRegions.push_back(region);
		}

	private:
		std::uint16_t numberOfCOs{0};
		std::uint16_t stackFrameIndex{0};	   ///< current frame within a stack sequence, zero-based indexing [0-N]
		float CFAR{0};						   ///< The average per-pixel false alarm rate for the sensor (FA/pixel)
		std::uint16_t validityFlagBitField{0}; ///< validity of stackFrameIndex and CFAR
		std::chrono::nanoseconds TOVutcNanoseconds{0};
		std::vector<HotRegion> hotRegions;	   ///< Position and dimensions of large, extended, high intensity objects in the FOV
	};
} // namespace ams::iface::irmel
