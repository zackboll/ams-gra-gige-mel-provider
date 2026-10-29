//===============================================================================
/// @file  CommonIR_MEL.h
/// @brief This file includes the common type definitions for the IR MEL

#pragma once

#include <irmel/library/irmel-types/IR_Directional.h>
#include <irmel/library/irmel-types/mel_export.h>
#include <mel/library/CommonMEL.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

/**
 * @(#)IRMEL.idl
 */
/**
 * @class API_Manager
 * @brief The API_Manager interface serves as an instance manager for MEL interfaces.
 */
/// @Required This class supports IR MEL library loading and must be included as-is in all IR MEL
/// implementations.
class API_Manager
{
public:
	virtual ~API_Manager() = default;

	API_Manager() = default;
	API_Manager(const API_Manager&) = delete;
	API_Manager& operator=(API_Manager&) = delete;
	API_Manager(API_Manager&& other) = delete;
	API_Manager& operator=(API_Manager&& other) = delete;
};

// extern "C" of a function that returns a shared_ptr is not supported by clang as it
//    notes that if C called this function, it would have trouble converting a shared_ptr
//    to C as there is no translation. This warning is being ignored as this function
//    is not being called by C, but requires C-Linkage for dlopen and dlsym calls as a result
//    of the dynamic library loading in LibraryLoader.cpp
/// @brief Get an instances manager for MEL interfaces
/// @Required This function supports IR MEL library loading and must be included as-is in all IR MEL
/// implementations.
#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
extern "C" std::shared_ptr<API_Manager> getAPI_Manager(const std::string& instance);
#if __clang__
#pragma clang diagnostic pop
#endif
/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
/// Of interest is the Command Type enum class,
/// which documents all commands that can be sent from the MFP to the MFA.
/// The Classes in the file
/// describe how the connections between the MFA and MFP are made.
namespace ams::iface::irmel
{
	///< These tasks occur within the MFA and are lower level than the ExecuteCmd commands,
	///< but are performed in response to those commands and reported out for status
	/// @enum Task
	/// @brief Indicates tasks that are performed in response to commands executed
	/// @Required This enumeration class defines data used in IR MEL function defintions to indicate the task
	/// and must be included as-is in all IR MEL implementations.
	enum class Task : std::uint32_t
	{
		NoTask,
		AbortTask,
		IRSTCamCal,
		CamCal,
		IRSTBoresight,
		Idle, ///< to Map into TCB::SD TimelineMgmt::DecisionTree in TimelineMgmt.cpp
		IRSTScan,
		IRSTPosition,
		IRSTSTT,
		IRSTMTT,
		IRSTUpdate,
		Reserved1,
		Reserved2,
		Reserved3,
		Reserved4,
		Reserved5,
		Reserved6,
		NumTaskTypes ///< This should always be last
	};

	/// @enum BadPixelReason
	/// @brief Denotes the reason for a reported bad pixel, if known
	/// @Required This enumeration provides data defintion associated with reporting bad pixels,
	/// which is required functionality and must be included as-is in all IR MEL implementations
	enum class BadPixelReason : std::uint32_t
	{
		Unknown
	};

	/// @enum MFA_Mode
	/// @brief Most MFA_States do not contain modes, and use the Unused Mode.
	/// The Operational State contains 3 modes.
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate operational states
	/// and must be included as-is in all IR MEL implementation
	enum class MFA_Mode : std::uint32_t
	{
		Unused,			 ///< ModeCmd when switching to a MFA_State that does not have any modes within it
		TaskSched,		 ///< Operational State Only - All MFAs support this mode.
		ScanVolumeSched, ///< Operational State Only
		ScanBarSched	 ///< Operational State Only - Growth Mode
	};

	/// @enum CommandState
	/// @brief CommandState aligns with the OMS CommandState
	/// @Required This enumeration class defines data used in IR MEL function defintions to indicate command status
	/// and must be included as-is in all IR MEL implementations
	enum class CommandState : std::uint32_t
	{
		NotSet,
		/// Indicates a [NEW] Command has been received and the Subsystem is determining if it can perform the Command.
		/// An [UPDATE] Command can be received in this state which will modify the command parameters.
		Received,
		/// Indicates the Subsystem has accepted the Command and will subsequently status it through
		/// [Capability]Activity or other another status message.  This is a terminal state and the Subsystem
		/// will ignore all subsequent updates to the Command, including DELETE.
		Accepted,
		Rejected,
		/// Indicates a [DELETE] Command was been received while the Subsystem could honor it and the
		/// Command was removed from consideration with no resulting Activity and/or functionality starting.
		Cancelled
	};

	/// @enum CannotComply
	/// @brief Indicates the action (Task, [Capability]Command, [SupportingCapability]Command,
	/// [Capability]Activity, etc.) was rejected, interrupted, unallocated, failed or generally can't be
	/// completed satisfactorily because... <see fields for reason>
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate actions
	/// and must be included as-is in all IR MEL implementations
	enum class CannotComply : std::uint32_t
	{
		NotSet,
		/// because the number of attempts specified in the command was reached before the action
		/// achieved the desired result.
		ConstraintAttempts,
		/// because of insufficient endurance.  For example, fuel or battery limitations.
		/// This is a generalization of the former INSUFFICIENT_FUEL_QUANTITY indication.
		ConstraintEndurance,
		/// because the classification of the action, actor, capability or
		/// associated elements is inconsistent.
		ConstraintClassification,
		/// because it exceeds field of regard (FOR)
		/// or field of view (FOV) limitations.
		ConstraintFORFOVLimit,
		/// because of a gating constraint on the command.  For example, the target of the
		/// action didn't meet velocity, range, power, etc. gates/filters in the command.
		ConstraintGating,
		ConstraintManeuverLimit, ///< because maneuvering exceeds maneuver limits of the action.
		/// because of a constraint imposed by an "Op" message (OpZone, OpLine, OpRouting, etc.).
		/// For example, a Strike Task in a NO_FIRE OpZone.  This is a generalization of the previous
		/// NO_FIRE or NO_IMAGE indications.
		ConstraintOp,
		ConstraintOcclusion, ///< because of line of sight limitations due to terrain occlusion
		/// because of a range related limitation of the associated Capability.
		/// This is a generalization of the former INSUFFICIENT_SENSOR_RANGE indication.
		CapabilityRange,
		CapabilityPerformance,
		/// because of a conflict between the needed/commanded RF parameters and the RF
		/// spectrum available.  For example, the commanded RF band isn't supported by
		/// the loaded MDF of a Capability or there is an active RF_Profile that doesn't
		/// allow transmission in the commanded band.  This is a generalization of the former
		/// INVALID_RF_BAND, RF_SPECTRUM_LIMITED, RF_UNAVAILABLE and RF_PROFILE_RESTRICTION.
		ConstraintRF,
		/// because it isn't achievable given the currently planned route.  Alternatively, a
		/// route to achieve the action couldn't be generated.
		ConstraintRoute,
		ConstraintSafety, ///< because of a safety related constraint.
		/// because of command constraints on the angle between
		/// the actor and the target of the action.
		ConstraintTargetAngle,
		/// because it has temporal requirements that can't be achieved or were reached
		/// before the action achieved the desired result.
		ConstraintTime,
		/// because it was constrained for allocation to a specific System that cannot complete it.
		/// This is a generalization of the previous INELIGIBLE_VEHICLE indication.
		ConstraintSystem,
		InfeasibleRoute, ///< because a route to achieve the action couldn't be generated.
		/// due to a general mission event (flight contingency, mission contingency, allocation
		/// change, etc.).  This is generalization of the previous MISSION_EVENT indication.
		MissionEvent,
		/// because of current/future state or settings of the
		/// corresponding Capability or Subsystem.
		StateOrSettings,
		///  because a change in the state or settings of the
		/// corresponding Capability or Subsystem.
		StateOrSettingsChange,
		SystemUnavailable, ///< because the associated System wasn't available at the needed time.
		SystemFault,	   ///< because the associated System is faulted.
		/// because the action results in conflict with another System's route, sensor,
		/// weapon or other plans/actions.
		SystemConflict,
		SubsystemUnavailable, ///< because the associated Subsystem wasn't available at the needed time.
		SubsystemFault,		  ///< because the associated Subsystem is faulted.
		CapabilityFault,	  ///< because the associated Capability is faulted.
		/// because the precedence of its associated Capability within the associated
		/// Subsystem wasn't high enough for the action to proceed.
		CapabilityPrecedence,
		CapabilityUnavailable, ///< because the associated Capability wasn't available at the needed time.
		/// because of insufficient resources (e.g. signal processors, memory or other
		/// items generally subordinate to Subsystems and Capabilities).  In this condition,
		/// the associated Capability, Subsystem and System should be otherwise able to perform the action.
		InsufficientResources,
		/// because its ranking wasn't high enough to gain or maintain
		/// access to the required capability.
		Ranking,
		Weather, ///< because of weather effects.
		/// because the source of the action isn't eligible and/or hasn't been granted
		/// permission to control (mission control, primary capability control, secondary
		/// capability control, automated capability management) the associated System or Capability.
		/// This is a generalization of the former SYSTEM_NOT_IN_CONTROL and SERVICE_NOT_IN_CONTROL.
		IneligibleControlSource,
		/// because a required predecessor action wasn't allocated, unallocated, failed, etc.
		/// This is a generalization of the former UNASSIGNABLE_DEPENDENT_TASK.
		DependencyPredecessor,
		/// because it has an "all or nothing" association with another action that wasn't allocated,
		/// unallocated, failed, etc.  This is a generalization of the former ALL_OR_NOTHING_TASK_CONSTRAINT.
		DependencyAllOrNothing,
		/// because it has an "either or" association with another action that wasn't allocated,
		/// unallocated, failed, etc.  This is a generalization of the former EITHER_OR_TASK_CONSTRAINT.
		DependencyEitherOr,
		InitCriteriaNotMet, ///< because the initialization conditions were not met.
		/// because the object corresponding to an ID in the
		/// command could not be found.
		UnknownID,
		InvalidInputParameter, ///< because a parameter in the action or other associated input was invalid.
		InputOther,			   ///< because of another unspecified input problem.
		/// Indicates the action to activate an MDF ([Capability]SettingsCommand was rejected
		/// because the commanded MDF_ID and/or SubCategory FileID is invalid or represents a file
		/// set that is not compliant with the Capability.
		MDFActivationError,
		/// because of multiple sibling enumeration reasons but a
		/// specific single reason isn't known.
		Multiple,
		/// Indicates the action (Task, [Capability]Command, [SupportingCapability]Command,
		/// [Capability]Activity, etc.) was cancelled, generally by the originating source.
		/// This is a generalization of the previous USER_CANCELLED indication.
		Cancelled,
		Other,	 ///< because of another reason which hasn't yet been incorporated into the enumeration.
		Unknown, ///< because of an unknown reason.
		/// Indicates the action (Task, [Capability]Command, [SupportingCapability]Command,
		/// [Capability]Activity, etc.) was aborted due to constraints and/or priorities of the
		/// mission or system.  This is different from an action that was cancelled, rejected, or failed.
		Aborted,
		AlignmentManeuver
	};

	/// @enum TaskCompStatus
	/// @brief Indicates the status of a completed task
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate task states
	/// and must be included as-is in all IR MEL implementations
	enum class TaskCompStatus : std::uint32_t
	{
		NoTask,
		Complete,
		Timeout,
		Abort,
		Skipped
	};

	/// @enum TaskListAction
	/// @brief Used during TaskScheduling mode to affect the current task or
	/// to assign priority for queued tasks if TaskScheduleDepth>0
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate task list actions
	/// and must be included as-is in all IR MEL implementaions
	enum class TaskListAction : std::uint32_t
	{
		Normal, ///< Adds the task into the queue based on the priority value
		/// If multiple skills have registered on this channel then Next, Immediate, and Abort are illegal
		/// The MFA will respond with an error code and drop the task
		Next, ///< Adds the task to the front of the queue keep the current queue intact
		/// Immediate might be allowed if MPS has a high priority message for lower priority skill
		Immediate, ///< aborts the current task and clears the queue
		/// Abort might be allowed if trying to abort its own tasks in the queue?
		Abort ///< aborts the current task
	};

	/// @enum Failure
	/// @brief Indicates the failure state for a task
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate failure states
	/// and must be included as-is in all IR MEL implementations
	enum class Failure : std::uint32_t
	{
		NA,
		Critical, ///< No heartbeat message
		Major,
		Parametric,
		Informational,
		Available, ///< No failure
		/// Used to indicate that the subsystem isn't even
		/// physically installed (or simulated-ly installed)
		NotPresent
	};
	/// @enum CSCIMode
	/// @brief Indicates modes during configuration of Subsystem
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate mode configuration
	/// and must be included as-is in all IR MEL implementations
	enum class CSCIMode : std::uint32_t
	{
		Unknown,
		Unused,
		Initialization,
		Maintenance,
		Idle,
		Operational,
		VSA,
		QuickLook,
		Track,
		Imaging,
		Noise
	};

	/// @enum IrstTrackState
	/// @brief Indicates the state of the Irst Track Report
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate track states
	/// and must be included as-is in all IR MEL implementations
	enum class IrstTrackState : std::uint32_t
	{
		Idle,
		Detected,
		Coast,
		Dropped
	};

	/// @enum IrstTrackMode
	/// @brief Indicates the mode of the Irst Track Report
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate track mode
	/// and must be included as-is in all IR MEL implementations
	enum class IrstTrackMode : std::uint32_t
	{
		Idle,
		Scan,
		Stare
	};

	/// @enum Priority
	/// @brief Indicates the priority of a task whether it is normal of debug
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate task priority
	/// and must be included as-is in all IR MEL implementations
	enum class Priority : std::uint32_t
	{
		Normal,
		Debug
	};

	/// @enum ImageType
	/// @brief Image type indicating whether image is starring or scanning
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate image types
	/// and must be included as-is in all IR MEL implementations
	enum class ImageType : std::uint32_t
	{
		Staring,
		Scanning,
		Reserved13
	};

	/// @enum ScanDir
	/// @brief Scan Directory indicating whther direction is positive to negative
	/// or negative to positive
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate direction
	/// and must be included as-is in all IR MEL implementations
	enum class ScanDir : std::uint32_t
	{
		NegToPosAz,
		PosToNegAz
	};

	/// @enum CoordFrameRef
	/// @brief Indicates which vehicle receives the frame reference command
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate vehicle frame reference
	/// and must be included as-is in all IR MEL implementations
	enum class CoordFrameRef : std::uint32_t
	{
		Inertial,
		Aircraft
	};

	/// @enum DegredationMethod
	/// @brief Indicates method used to handle schedule capacity limits
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate degredation methods
	/// and must be included as-is in all IR MEL implementations
	enum class DegradationMethod : std::uint32_t
	{
		CAPACITY_DEGRADATION, ///< no further tasking scheduled
		VOLUME_DEGRADATION,	  ///< azimuth extent is reduced to allow more to be scheduled
		RANGE_DEGRADATION,	  ///< scan rate is increased to allow more to be scheduled
		REVISIT_DEGRADATION	  ///< required revisit rate is increased to allow more to be scheduled
	};

	// used in test fixture!  Resolve another method
	const std::string IR_MEL_API_VERSION = "4.0";

	/// @enum Return
	/// @brief Return types for IR MEL operations
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate operation returns
	/// and must be included as-is in all IR MEL implementations
	enum class Return : std::uint32_t
	{
		Success,
		BadPointer,
		Fail,
		/// This literal indicates that the command is not supported by this implementation
		/// of the IR MEL, notionally by the MFA that implements the IR MEL.
		NotSupported,
		/// This literal indicates that the command is supported but not implemented by this
		/// implementation of the IR MEL.
		NotImplemented
	};

	/// @enum BufferFlag
	/// @brief Indication of buffers that can be flagged
	/// @Required This enumeration class defines data used in IR MEL function definitions to indicate buffer flags
	/// and must be included as-is in all IR MEL implementations
	enum class BufferFlag : std::uint32_t
	{
		Host,
		GPU,
		RDMA
	};

	/// @enum PixelFormat
	/// @brief Indicates the pixel format of a frame
	/// @Required This enumeration provides data defintion associated with producing frame data,
	/// which is required functionality and must be included as-is in all IR MEL implementations
	enum class PixelFormat : std::uint32_t
	{
		Mono,
		RGB,
		Bayer
	};

	/// @enum ImageFlip
	/// @brief Indicates the flip(s) applied to a frame
	/// @note Handled prior to applying distortion map
	/// @Required This enumeration provides data defintion associated with producing frame data,
	/// which is required functionality and must be included as-is in all IR MEL implementations
	enum class ImageFlip : std::uint32_t
	{
		None,
		Vertical,
		Horizontal,
		Both
	};

	/// @enum ImageFlag
	/// @brief Indicates the scan/stare behavior employed by the MFA while producing a frame
	/// @Required This enumeration provides data defintion associated with producing frame data,
	/// which is required functionality and must be included as-is in all IR MEL implementations
	enum class ImageFlag : std::uint32_t
	{
		ScanFirst,	   ///< For scanning IR MFA, indicates that the image chunk is the first in the scan
		ScanLast,	   ///< For scanning IR MFA, indicates that the image chunk is the last in the scan
		StareSnapshot, ///< For staring image, indicates that the image was in a shapshot frame capture
		StareRolling   ///< For staring image, indicates that the image was in a rolling shutter frame capture
	};

	/// @brief Allow for Sensor IDs to be from 0 to 2^32-1
	/// @Required This type provides an ID for sensors and must be included as-is in all IR MEL implementations
	using SensorIDType = std::uint32_t;

	/// @brief Type for listing of the sensor face of an MFA associated with a frame of image data
	/// @Required This type associates a sensor ID with a sensor face of an MFA and must be included
	/// as-is in all IR MEL implementations
	using ContributingSensor = std::pair<ams::iface::mel::ComponentLocation, SensorIDType>;

	/// @enum SensorType
	/// @brief Indicates the sensor type associated with a channel
	/// @Required This enumeration provides data definition associated with reporting channel capabilities,
	/// which is required functionality and must be included as-is in all IR MEL implementations
	enum class SensorType : std::uint32_t
	{
		Unspecified,	   ///< Uknown sensor capabilities
		GIMBAL_HORIZONTAL, ///< Sensor able to pivot about the horizontal axis
		GIMBAL_VERTICAL,   ///< Sensor able to pivot about the vertical axis
		GIMBAL_ROTATION,   ///< Sensor able to rotate 360 degrees
		STEPSTARE,		   ///< Step-stare mechanization used for scanning
		MAXEXCLUSIVE	   ///< Unique capabilites defined beyond this point
	};

} // end namespace ams::iface::irmel
  // end interface ControlObject
  // end module IR_MEL
