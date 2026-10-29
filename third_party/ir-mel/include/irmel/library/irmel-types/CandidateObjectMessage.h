//===============================================================================
/// @file  CandidateObjectMessage.h
/// @brief This file includes the CandidateObjectMessage class definition.

#pragma once

#include <irmel/library/irmel-types/CandidateObject.h>
#include <irmel/library/irmel-types/CandidateObjectHeader.h>
#include <irmel/library/irmel-types/SensorInertialState.h>
#include <array>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{

	static constexpr std::uint32_t MAX_CANDIDATE_OBJECTS = 900;

	/// @class CandidateObjectMessage
	/// @brief Receives a message to get an candidate object
	/// @RequiredIfBuiltInTracker This class provides data definition in support of required IR MEL functionality to receive candidate object
	/// and must be included as-is in all IR MEL implementations
	class CandidateObjectMessage
	{
	public:
		CandidateObjectMessage() = default;
		~CandidateObjectMessage() = default;
		CandidateObjectMessage(const CandidateObjectMessage&) = default;
		CandidateObjectMessage(CandidateObjectMessage&&) = default;
		CandidateObjectMessage& operator=(const CandidateObjectMessage&) = default;
		CandidateObjectMessage& operator=(CandidateObjectMessage&&) = default;

		[[nodiscard]] const CandidateObjectHeader& getHeader() const
		{
			return this->header;
		}
		void setHeader(CandidateObjectHeader header_in)
		{
			this->header = header_in;
		}
		[[nodiscard]] const SensorInertialState& getInertialState() const
		{
			return this->inertialState;
		}
		void setInertialState(const SensorInertialState& inertialState_in)
		{
			this->inertialState = inertialState_in;
		}
		[[nodiscard]] const std::array<CandidateObject, MAX_CANDIDATE_OBJECTS>& getCandidateObjects() const
		{
			return this->candidateObjects;
		}
		void setCandidateObjects(const std::array<CandidateObject, MAX_CANDIDATE_OBJECTS>& candidateObjects_in)
		{
			this->candidateObjects = candidateObjects_in;
		}

	private:
		CandidateObjectHeader header;
		SensorInertialState inertialState;
		std::array<CandidateObject, MAX_CANDIDATE_OBJECTS> candidateObjects;
	};
} // namespace ams::iface::irmel
