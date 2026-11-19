//===============================================================================
/// @file  StackedImageChannel.h
/// @brief This file includes the Stacked Image Channel type.
//===============================================================================
#pragma once

#include <irmel/library/irmel-types/Channel.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/LOS3D_KinematicsType.h>
#include <mel/library/CommonMEL.h>
#include <functional>

namespace ams::iface::irmel
{
	/// @class StackedImageChannel
	/// @brief Stacked Image Channel used by services.
	/// @Required This class provides required IR MEL functionality for reporting channel details, and must be provided by the implementer
	/// in all IR MEL implementations. See individual members for details
	class StackedImageChannel : virtual public Channel
	{
	public:
		/// @Required This function supports channel and must be provided by the implementer for all IR MEL implementations
		~StackedImageChannel() override = default;

		/// @brief Callback Registration for LOS3D_Kinematics
		/// @param callback std::function callback for LOS3D_Kinematics
		/// @return Return::NotSupported if not implemented, Return::Success if implemented and registration succeeds, Return::Fail if a
		///   callback is already registered for this datatype on this channel
		/// @Optional This function supports callback registration and is optional for IR MEL implementations
		virtual Return registerMetadataCallback(std::function<void(Channel& channel, const LOS3D_KinematicsType* const)> const& callback) = 0;

		using Channel::registerMetadataCallback;
		using Channel::send;

		StackedImageChannel() = default;
		StackedImageChannel(const StackedImageChannel&) = delete;
		StackedImageChannel& operator=(StackedImageChannel&) = delete;
		StackedImageChannel(StackedImageChannel&& other) = delete;
		StackedImageChannel& operator=(StackedImageChannel&& other) = delete;
	};
	// end interface StackedImageChannel

} // end namespace ams::iface::irmel
