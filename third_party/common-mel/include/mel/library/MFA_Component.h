#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <utility>

#include "Directional.h"

namespace ams::iface::mel
{
	/// @class About
	/// @brief Indicates version numbers associated with the Subsystem
	/// and its software and hardware. This type is aligned with the OMS/ UCI AboutType.
	/// @Required This class provides data definition in support of required MEL functionality to report version
	/// details of the Subsytem and must be included as-is in all MEL implementations.
	class About
	{
	public:
		About() = default;
		About(std::string mod, std::string snum, std::string sver, std::string bootver, std::string hver)
			: model{std::move(mod)},
			  serialNumber{std::move(snum)},
			  softwareVersion{std::move(sver)},
			  bootloaderSoftwareVersion{std::move(bootver)},
			  hardwareVersion{std::move(hver)}
		{
		}
		~About() = default;
		About(const About&) = default;
		About(About&&) = default;
		About& operator=(const About&) = default;
		About& operator=(About&&) = default;

		[[nodiscard]] const std::string& getModel() const
		{
			return this->model;
		}

		void setModel(const std::string& newValue)
		{
			this->model = newValue;
		}

		[[nodiscard]] const std::string& getSerialNumber() const
		{
			return this->serialNumber;
		}

		void setSerialNumber(const std::string& newValue)
		{
			this->serialNumber = newValue;
		}

		[[nodiscard]] const std::string& getSoftwareVersion() const
		{
			return this->softwareVersion;
		}

		void setSoftwareVersion(const std::string& newValue)
		{
			this->softwareVersion = newValue;
		}

		[[nodiscard]] const std::string& getBootloaderSoftwareVersion() const
		{
			return this->bootloaderSoftwareVersion;
		}

		void setBootloaderSoftwareVersion(const std::string& newValue)
		{
			this->bootloaderSoftwareVersion = newValue;
		}

		[[nodiscard]] const std::string& getHardwareVersion() const
		{
			return this->hardwareVersion;
		}

		void setHardwareVersion(const std::string& newValue)
		{
			this->hardwareVersion = newValue;
		}

	private:
		/// Indicates a widely known and coordinated model name for the associated item, generally
		/// established at the time of production.  For example, "AN/APG-79".
		std::string model;
		std::string serialNumber;	 ///< unique identifier assigned to an instance of the Model
		std::string softwareVersion; ///< software/OFP version, for items whose software can be updated post-production without changing
									 ///< the Model.
		std::string bootloaderSoftwareVersion;
		std::string hardwareVersion; ///< hardware version, for items whose hardware can be updated post-production without changing the Model.
	};

	/// @enum MFA_State
	/// @brief Indicates the overall status/state of the Subsystem.  See enumeration annotations for further details.
	/// This enum aligns with the OMS states, except it excludes OperateTxOnly because that state does not
	/// exist for an EOIR sensor.
	/// @Required This enumeration class defines data used in MEL function definitions to indicate MFA operating states
	/// and must be included as-is in all MEL implementations.
	enum class MFA_State : std::uint32_t
	{
		/// enum has not been set
		NotSet,
		/// subsystem is in an unknown state
		Unknown,
		/// subsystem is not installed
		Not_Installed,
		/// not powered up
		Off,
		/// performing bootstrap and initial OFP load.
		Pre_initialization,
		/// performing initialization and mission data load.
		Initialization,
		/// completed its initialization and is ready to transition to the operation state.
		Standby,
		/// allowed to perform its enabled capabilities.
		Operate,
		/// Indicates the subsystem is allowed to perform its enabled receive only capabilities.
		OperateRxOnly,
		/// MFA is in OperateTxOnly,
		OperateTxOnly,
		/// performing or has performed its maintenance procedure.
		Maintenance,
		/// Indicates the subsystem state of calibration.  This form of calibration is initiated
		/// via SubsystemStateCommand and interrupts other functions.  Some subsystems support a form
		/// of calibration that is initiated via SubsystemBIT_Command, doesn't require a subsystem state
		/// change and doesn't interrupt other subsystem functions.  See SubsystemCalibrationConfiguration
		/// for further details regarding the difference between types of commanded/initiated calibration.
		Calibration,
		/// Indicates the subsystem state of Initiated Built In Test (BIT).  This form of initiated BIT
		/// is initiated via SubsystemStateCommand and interrupts other functions.  Some subsystems support
		/// a form of initiated BIT that is initiated via SubsystemBIT_Command, doesn't require a subsystem
		/// state change and doesn't interrupt other subsystem functions.  See SubsystemBIT_Configuration
		/// for further details regarding the difference between types of commanded/initiated BIT.
		Initiated_BIT,
		/// is performing or has performed its shutdown procedure. Once complete, no messages will be sent by the subsystem.
		Shutdown,
		/// subsystem cannot perform all normal activities.
		Degraded,
		/// maximun enum item
		MaxExclusive
	};

	/// @enum StateTransitionStatus
	/// @brief Indicates whether or not the Subsystem is transitioning between states,
	/// or transitioning to its SHUTDOWN state.
	/// @Required This enumeration class defines data used in MEL function definitions to indicate MFA transition states
	/// and must be included as-is in all MEL implementations.
	enum class StateTransitionStatus : std::uint32_t
	{
		NotSet,		   ///< enum has not been set
		NotTransitioning,
		ShuttingDown,  ///< Subsystem is actively in the process of transitioning to its SHUTDOWN state
		Transitioning, ///< Indicates Subsystem is actively in the process of transitioning to another state other than SHUTDOWN
		MaxExclusive   ///< maximun enum item
	};

	/// @enum ComponentState
	/// @brief Indicates the state of the Subsystem Component.
	/// @Required This enumeration class defines data used in MEL function definitions to indicate Subsystem operating states
	/// and must be included as-is in all MEL implementations.
	enum class ComponentState : std::uint32_t
	{
		NotSet, ///< enum has not been set
		Unknown,
		NotInstalled,
		Off,		  ///< installed, but not powered on.
		Initializing, ///< initializing hardware components or software elements.
		Operational,  ///< able to perform advertised capabilities.
		Degraded,	  ///< degraded but still able to support at least one capability.
		Disabled,	  ///< not enabled to perform its capabilities or has been disabled for some reason.
		Faulted,	  ///< incapable of supporting any capabilities.
		MaxExclusive  ///< maximun enum item
	};

	/// @enum TemperatureState
	/// @brief Indicate the temperature status for a subsystem or subsystem component.
	/// @Required This enumeration class defines data used in MEL function definitions to indicate component temperature states
	/// and must be included as-is in all MEL implementations.
	enum class TemperatureState : std::uint32_t
	{
		NotSet,			  ///< enum has not been set
		UnderTemp,		  ///< temperature is below normal operating range
		Normal,			  ///< normal operating range
		OverTempWarning,  ///< higher than the normal operating range but has not yet affected component operation
		OverTempDegraded, ///< Component is operating in a degraded mode due to high component temperature.
		OverTempShutdown, ///< Component has shut down due to high component temperature.
		MaxExclusive	  ///< maximun enum item
	};

	/// @class TemperatureStatus
	/// @brief Indicates the temperature of the Subsystem Component corresponding to this message.
	/// This type is aligned with the OMS/ UCI TemperatureStatusType.
	/// @Required This class provides data definition in support of required MEL functionality to report component
	/// temperature status and must be included as-is in all MEL implementations.
	class TemperatureStatus
	{
	public:
		TemperatureStatus() = default;
		TemperatureStatus(double temp, TemperatureState state) : temperature{temp}, temperatureState{state}
		{
		}
		~TemperatureStatus() = default;
		TemperatureStatus(const TemperatureStatus&) = default;
		TemperatureStatus(TemperatureStatus&&) = default;
		TemperatureStatus& operator=(const TemperatureStatus&) = default;
		TemperatureStatus& operator=(TemperatureStatus&&) = default;

		[[nodiscard]] double getTemperature() const
		{
			return this->temperature;
		}

		void setTemperature(double newValue)
		{
			this->temperature = newValue;
		}

		[[nodiscard]] const TemperatureState& getTemperatureState() const
		{
			return this->temperatureState;
		}

		void setTemperatureState(TemperatureState newValue)
		{
			this->temperatureState = newValue;
		}

	private:
		double temperature{0}; ///<  degrees Celsius.
		TemperatureState temperatureState{TemperatureState::NotSet};
	};

	/// @class InstallationDetails
	/// @brief Physical Installation Details
	/// @Required This class provides data definition in support of required MEL functionality to report
	/// installation details of the Subsytem and must be included as-is in all MEL implementations.
	class InstallationDetails
	{
	public:
		InstallationDetails() = default;
		InstallationDetails(ComponentLocation l, const Euler& o, const Euler& b) : location{std::move(l)}, orientation{o}, boresight{b}
		{
		}
		~InstallationDetails() = default;
		InstallationDetails(const InstallationDetails&) = default;
		InstallationDetails(InstallationDetails&&) = default;
		InstallationDetails& operator=(const InstallationDetails&) = default;
		InstallationDetails& operator=(InstallationDetails&&) = default;

		[[nodiscard]] const ComponentLocation& getLocation() const
		{
			return this->location;
		}

		void setLocation(const ComponentLocation& newValue)
		{
			this->location = newValue;
		}

		[[nodiscard]] const Euler& getOrientation() const
		{
			return this->orientation;
		}

		void setOrientation(const Euler& newValue)
		{
			this->orientation = newValue;
		}

		[[nodiscard]] const Euler& getBoresight() const
		{
			return this->boresight;
		}

		void setBoresight(const Euler& newValue)
		{
			this->boresight = newValue;
		}

	private:
		ComponentLocation location;
		Euler orientation;
		Euler boresight;
	};

	/// @class MFA_Component
	/// @brief Contains component status information.  See individual field annotations for more information.
	/// @Required This class provides data definition in support of required MEL functionality to report status
	/// details of an MFA Component and must be included as-is in all MEL implementations.
	class MFA_Component
	{
	public:
		MFA_Component() = default;
		MFA_Component(UCI_ID id, ComponentState state, TemperatureStatus status, ForeignKey key, InstallationDetails detail)
			: componentId{std::move(id)},
			  componentState{state},
			  temperature{status},
			  installationLocationId{std::move(key)},
			  installationDetails{std::move(detail)}
		{
		}
		~MFA_Component() = default;
		MFA_Component(const MFA_Component&) = default;
		MFA_Component(MFA_Component&&) = default;
		MFA_Component& operator=(const MFA_Component&) = default;
		MFA_Component& operator=(MFA_Component&&) = default;

		[[nodiscard]] const UCI_ID& getComponentId() const
		{
			return this->componentId;
		}

		void setComponentId(const UCI_ID& newValue)
		{
			this->componentId = newValue;
		}

		[[nodiscard]] const ComponentState& getComponentState() const
		{
			return this->componentState;
		}

		void setComponentState(ComponentState newValue)
		{
			this->componentState = newValue;
		}

		[[nodiscard]] const TemperatureStatus& getTemperature() const
		{
			return this->temperature;
		}

		void setTemperature(TemperatureStatus newValue)
		{
			this->temperature = newValue;
		}

		[[nodiscard]] const ForeignKey& getInstallationLocationId() const
		{
			return this->installationLocationId;
		}

		void setInstallationLocationId(const ForeignKey& newValue)
		{
			this->installationLocationId = newValue;
		}

		[[nodiscard]] const InstallationDetails& getInstallationDetails() const
		{
			return this->installationDetails;
		}

		void setInstallationDetails(const InstallationDetails& newValue)
		{
			this->installationDetails = newValue;
		}

	private:
		UCI_ID componentId;						 ///< unique ID of the Subsystem Component corresponding to this message.
		ComponentState componentState{0};		 ///< current operational status of the resource.
		TemperatureStatus temperature{};
		ForeignKey installationLocationId;		 ///< The ID of the logical location where the component is mounted on the Subsystem.
		InstallationDetails installationDetails; ///< physical installation details
	};

	/// @class MFA_Status
	/// @brief Indicates an instance of a Subsystem Status of the vehicle reporting its readiness. This type is aligned with the OMS/ UCI
	/// SubsystemStatusMDT.
	/// @Required This class provides data definition in support of required MEL functionality to report Subsytem Status
	/// and must be included as-is in all MEL implementations.
	class MFA_Status
	{
	public:
		MFA_Status() = default;
		MFA_Status(MFA_State state, std::string statedesc, std::string modedesc, StateTransitionStatus status, About ab,
				   std::vector<MFA_Component> comps)
			: MFAState{state},
			  MFAStateDescription{std::move(statedesc)},
			  MFAModeDescription{std::move(modedesc)},
			  stateTransitionStatus{status},
			  about{std::move(ab)},
			  MFAComponents{std::move(comps)}
		{
		}
		~MFA_Status() = default;
		MFA_Status(const MFA_Status&) = default;
		MFA_Status(MFA_Status&&) = default;
		MFA_Status& operator=(const MFA_Status&) = default;
		MFA_Status& operator=(MFA_Status&&) = default;

		[[nodiscard]] const MFA_State& getMFAState() const
		{
			return this->MFAState;
		}

		void setMFAState(MFA_State newValue)
		{
			this->MFAState = newValue;
		}

		[[nodiscard]] const std::string& getMFAStateDescription() const
		{
			return this->MFAStateDescription;
		}

		void setMFAStateDescription(const std::string& newValue)
		{
			this->MFAStateDescription = newValue;
		}

		[[nodiscard]] const std::string& getMFAModeDescription() const
		{
			return this->MFAModeDescription;
		}

		void setMFAModeDescription(const std::string& newValue)
		{
			this->MFAModeDescription = newValue;
		}

		[[nodiscard]] const StateTransitionStatus& getStateTransitionStatus() const
		{
			return this->stateTransitionStatus;
		}

		void setStateTransitionStatus(StateTransitionStatus newValue)
		{
			this->stateTransitionStatus = newValue;
		}

		[[nodiscard]] const About& getAbout() const
		{
			return this->about;
		}

		void setAbout(const About& newValue)
		{
			this->about = newValue;
		}

		[[nodiscard]] const std::vector<MFA_Component>& getMFAComponents() const
		{
			return this->MFAComponents;
		}
		// replace the existing vector with a new vector

		void setMFAComponents(const std::vector<MFA_Component>& newValue)
		{
			this->MFAComponents = newValue;
		}

		// add a new element to the vector

		void addCapabilityID(const MFA_Component& comp)
		{
			this->MFAComponents.push_back(comp);
		}

	private:
		/// Indicates the current state of the Subsystem.  The state machine and allowable
		/// transitions for Subsystem states is described in supporting documentation.  A given
		/// Subsystem might not support all states.  Subsystem state can be controlled via
		/// SubsystemStateCommand, discrete/special signals and/or other means.  See enumeration
		/// annotations for further details.
		MFA_State MFAState{0};
		std::string MFAStateDescription; ///< human readable, supplemental description of the state.
		std::string MFAModeDescription; ///< human readable, supplemental description of the mode.
		StateTransitionStatus stateTransitionStatus{
			0};							///< whether or not the Subsystem is transitioning between states, or transitioning to its SHUTDOWN state.
		About about;					///< version numbers associated with software and hardware.
		/// Indicates an auxiliary or subordinate Component of this Subsystem.
		/// Components can be physical or functional.  Any Component that is relevant to the state,
		/// health and/or C2 of the Subsystem can be reported.
		std::vector<MFA_Component> MFAComponents;
	};
} // end namespace ams::iface::mel
