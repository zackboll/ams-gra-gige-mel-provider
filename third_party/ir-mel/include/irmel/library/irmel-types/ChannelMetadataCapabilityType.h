//===============================================================================
/// @file  ChannelMetadataCapabilityType.h
/// @brief This file includes an enum of the types of Channel Metadata Capabilities available to a
/// Channel.

#pragma once

#include <mel/library/CommonMEL.h>
#include <cstdint>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @enum ChannelMetadataCapabilityType
	/// @brief Indicates a type of metadata supported by a channel.
	/// Used to form the set of supported types in the ChannelCapability class.
	/// @note Each enum value should correspond to a registerMetadataCallback function of the Channel class
	/// @Required This enumeration provides data definition in support of reporting Channel Metadata type capabilities
	/// and must be included as-is in all IR MEL implementations.
	enum class ChannelMetadataCapabilityType : std::uint32_t
	{
		BadPixelList,				   ///< See Callback Registration for BadPixelList
		OpticalDistortionMap,		   ///< See Callback Registration for OpticalDistortionMap
		LFStatus,					   ///< See Callback Registration for LFStatus
		LineOfSightReport,			   ///< See Callback Registration for LineOfSightReport
		LineOfSightQuaternion,		   ///< See Callback Registration for LineOfSightQuaternion
		LineOfSightEuler,			   ///< See Callback Registration for LineOfSightEuler
		MFAStatus,					   ///< See Callback Registration for MFA_Status
		MFAStatusDetailed,			   ///< See Callback Registration for MFA_StatusDetailed
		BITConfiguration,			   ///< See Callback Registration for BIT_Configuration
		CommandStatus,				   ///< See Callback Registration for CommandStatus
		BITStatus,					   ///< See Callback Registration for BIT_Status
		CandidateObjectMessage,		   ///< See Callback Registration for CandidateObjectMessage
		TaskExecutingRep,			   ///< See Callback Registration for TaskExecutingRep
		SubsystemStatusResp,		   ///< See Callback Registration for SubsystemStatusResp
		ExecuteTaskAck,				   ///< See Callback Registration for ExecuteTaskAck
		SchedCreatedRep,			   ///< See Callback Registration for SchedCreatedRep
		IRSTTrackReport,			   ///< See Callback Registration for IRSTTrackReport
		ChannelCommsTestRep,		   ///< See Callback Registration for ChannelCommsTestRep
		CameraCommandResp,			   ///< See Callback Registration for CameraCommandResp
		CameraProtectCmdResp,		   ///< See Callback Registration for CameraProtectCmdResp
		InstrumentationReport,		   ///< See Callback Registration for InstrumentationReport
		NavigationReportResp,		   ///< See Callback Registration for NavigationReportResp
		RequestSystemTrackData,		   ///< See Callback Registration for RequestSystemTrackData
		UpdateTrackListResponse,	   ///< See Callback Registration for UpdateTrackListResponse
		LOS3DKinematicsType,		   ///< See Callback Registration for LOS3D_KinematicsType
		CandidateObjectPreProcMessage, ///< See Callback Registration for CandidateObjectPreProcMessage
		TaskEvents,					   ///< See Callback Registration for TaskEvents
		ScanPerformanceReport,		   ///< See Callback Registration for ScanPerformanceReport
		Reserved3,					   ///< Reserved for later usage
		Reserved5,					   ///< Reserved for later usage
		Reserved9,					   ///< Reserved for later usage
		Reserved10					   ///< Reserved for later usage
	};
} // end namespace ams::iface::irmel
