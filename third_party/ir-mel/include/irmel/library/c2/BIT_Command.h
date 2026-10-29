#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace ams::iface::irmel
{
	/// @class BIT_Command
	/// @note Unlike the OMS/ UCI SubsystemBIT_CommandMDT type from which this type is derived, there is no SubsystemID field. It is assumed that
	/// an instance of the IR MEL is initialized to know what subsystem it is interacting with, making a per-command subsystem ID redundant. @note
	/// The OMS/ UCI SubsystemBIT_CommandMDT type from which this type is derived, expects the user to only populate one of the three ID fields
	/// (initiateBIT_ID, cancelBIT_ID, or clearFaultCode) by way of an XML Choice type. Since such Choice types are implemented as Unions in C++,
	/// and unnecessarily complex, this type does not explicitly indicate a choice, and it is the responsibility of the user to only populate one.
	/// The behavior of the IR MEL in the event that more than one of these fields is populated is to handle only one of them, in the following
	/// order: initiateBIT_ID, cancelBIT_ID, clearFaultCode.
	/// @Required This class provides data definition in support of required IR MEL functionality to command BITs
	/// and must be included as-is in all IR MEL implementations.
	/// @brief  This message is used to command BIT or BIT-related functions. Some Subsystems only support commanded/initiated
	/// BIT via SubsystemStateCommand.This message is intended for command/initiated BIT that isn't a Subsystem state and generally
	/// doesn't interrupt other Subsystem functions. See SubsystemBIT_Configuration for further details regarding the difference between types
	/// of commanded/initiated BIT. Identifies specific BIT IDs or Fault codes relevant to this command.
	class BIT_Command
	{
	public:
		BIT_Command() = default;
		BIT_Command(std::uint32_t id, std::vector<std::uint32_t> init, std::vector<std::uint32_t> cancel, std::vector<std::string> code)
			: commandID{id}, initiateBIT_ID{std::move(init)}, cancelBIT_ID{std::move(cancel)}, clearFaultCode{std::move(code)}
		{
		}
		~BIT_Command() = default;
		BIT_Command(const BIT_Command&) = default;
		BIT_Command(BIT_Command&&) = default;
		BIT_Command& operator=(const BIT_Command&) = default;
		BIT_Command& operator=(BIT_Command&&) = default;

		[[nodiscard]] const std::uint32_t& getCommandID() const
		{
			return this->commandID;
		}

		void setCommandID(const std::uint32_t& newValue)
		{
			this->commandID = newValue;
		}

		[[nodiscard]] const std::vector<std::uint32_t>& getInitiateBIT_ID() const
		{
			return this->initiateBIT_ID;
		}

		// replace the existing vector with a new vector
		void setInitiateBIT_ID(const std::vector<std::uint32_t>& newValue)
		{
			this->initiateBIT_ID = newValue;
		}

		// add a new element to the vector
		void addInitiateBIT_ID(const std::uint32_t& id)
		{
			this->initiateBIT_ID.push_back(id);
		}

		[[nodiscard]] const std::vector<std::uint32_t>& getCancelBIT_ID() const
		{
			return this->cancelBIT_ID;
		}

		// replace the existing vector with a new vector
		void setCancelBIT_ID(const std::vector<std::uint32_t>& newValue)
		{
			this->cancelBIT_ID = newValue;
		}

		// add a new element to the vector
		void addCancelBIT_ID(const std::uint32_t& id)
		{
			this->cancelBIT_ID.push_back(id);
		}

		[[nodiscard]] const std::vector<std::string>& getClearFaultCode() const
		{
			return this->clearFaultCode;
		}

		// replace the existing vector with a new vector
		void setClearFaultCode(const std::vector<std::string>& newValue)
		{
			this->clearFaultCode = newValue;
		}

		// add a new element to the vector
		void addFaultCode(const std::string& id)
		{
			this->clearFaultCode.push_back(id);
		}

	private:
		std::uint32_t commandID{0};					 ///< unique ID of the SubsystemBIT_Command.
		std::vector<std::uint32_t> initiateBIT_ID{}; ///< unique ID of the BIT to initiate.
		std::vector<std::uint32_t> cancelBIT_ID{};	 ///< unique ID of the BIT to cancel.
		std::vector<std::string> clearFaultCode{};	 ///< "code" or name of a fault to clear.
	};
} // end namespace ams::iface::irmel
