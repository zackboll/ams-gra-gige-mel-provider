//===============================================================================
/// @file  CandidateObjectPreProcMessage.h
/// @brief This file includes the CandidateObjectPreProcMessage class definition.

#pragma once

#include <irmel/library/irmel-types/SensorInertialState.h>

#include <irmel/library/irmel-types/CandidateObjectHeader.h>
#include <irmel/library/irmel-types/CandidateObjectPreProc.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <cstdint>
#include <utility>
#include <vector>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class CandidateObjectPreProcMessage
	/// @brief Indicates a message shall be involved with an objects state
	/// @RequiredIfBuiltInTracker This class provides data definitions in support of required IR MEL functionality to report object message
	/// and must be included as-is in all IR MEL implementations
	class CandidateObjectPreProcMessage
	{
	public:
		CandidateObjectPreProcMessage() = default;
		~CandidateObjectPreProcMessage() = default;
		CandidateObjectPreProcMessage(const CandidateObjectPreProcMessage&) = default;
		CandidateObjectPreProcMessage(CandidateObjectPreProcMessage&&) = default;
		CandidateObjectPreProcMessage& operator=(const CandidateObjectPreProcMessage&) = default;
		CandidateObjectPreProcMessage& operator=(CandidateObjectPreProcMessage&&) = default;

		[[nodiscard]] CandidateObjectHeader getCandidateObjectHeader() const
		{
			return this->header;
		}
		[[nodiscard]] SensorInertialState getSensorInertialState() const
		{
			return this->inertialState;
		}
		[[nodiscard]] std::vector<CandidateObjectPreProc> getCandidateObjectPreProcs() const
		{
			return this->candidateObjectPreProcs;
		}

		void setCandidateObjectHeader(const CandidateObjectHeader& header_in)
		{
			this->header = header_in;
		}
		void setSensorInertialState(const SensorInertialState& inertialState_in)
		{
			this->inertialState = inertialState_in;
		}
		void setCandidateObjectPreProcs(std::vector<CandidateObjectPreProc> candidateObjectPreProcs_in)
		{
			this->candidateObjectPreProcs = std::move(candidateObjectPreProcs_in);
		}

	private:
		CandidateObjectHeader header{};
		SensorInertialState inertialState{};
		std::vector<CandidateObjectPreProc> candidateObjectPreProcs{};
	};
} // namespace ams::iface::irmel
