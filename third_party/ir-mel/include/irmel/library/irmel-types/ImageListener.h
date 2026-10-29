//===============================================================================
/// @file  ImageListener.h
/// @brief This file includes the class definition for the ImageListener

#pragma once

#include <irmel/library/irmel-types/Buffer.h>
#include <irmel/library/irmel-types/Channel.h>
#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <memory>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	class Channel;

	/// @class ImageListener
	/// This interface defines the operations that must be implemented by a service that is listening for image data over the MEL.
	/// @brief Reports operations implemented by a service listening for image data over MEL.
	/// @Required This class provides required functionality for receiving image
	/// data, and must be provided by a service in order to support receipt of
	/// image data from an IR MEL implementation. See individual members for
	/// details.
	class ImageListener
	{
	public:
		/// @Required This function supports ImageListener and must be provided by the implementer for all IR MEL implementations
		virtual ~ImageListener() = default;

		/// @brief Prototype of the image data produced callback.
		/// @note The MEL invokes this function via the ImageListener_obj provided to the MEL by the Service in Config_T when the Service
		/// attaches to the channel using the Control_obj::attachChannel() operation. The MEL uses a Buffer_obj registered with the
		/// associated Channel_obj to store the image data. The memory managed by the Buffer_obj is created by the Service and it is the
		/// Service's responsibility to indicate when it is finished with the Buffer_obj (i.e., the underlying memory) so that the MEL can
		/// reuse it.
		/// @param [in,out] channel The channel on which the image data was produced.
		/// @param [in] header The frame header data
		/// @param [in,out] buffer Handle to the frame buffer data
		/// @Required This function supports image data and must be provided by the implementer for all IR MEL implementations
		virtual void onImage(const Channel& channel, const FrameHeader& header, std::shared_ptr<Buffer> buffer) = 0;

		ImageListener() = default;
		ImageListener(const ImageListener&) = delete;
		ImageListener& operator=(ImageListener&) = delete;
		ImageListener(ImageListener&& other) = delete;
		ImageListener& operator=(ImageListener&& other) = delete;
	};
	// end interface ImageListener
} // end namespace ams::iface::irmel
