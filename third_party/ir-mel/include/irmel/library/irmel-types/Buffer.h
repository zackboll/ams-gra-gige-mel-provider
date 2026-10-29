//===============================================================================
/// @file  Buffer.h
/// @brief This file includes the details of the buffer associated with the channel

#pragma once

#include <cstdint>

#include <irmel/library/irmel-types/CommonIR_MEL.h>
#include <memory>
#include <string_view>
#include <vector>

/// @namespace ams::iface::irmel
/// This namespace contains all of the GRA functionality for the IR MEL
namespace ams::iface::irmel
{
	/// @class Buffer
	/// @brief Gets details about buffer received
	/// @Required This class provides required IR MEL functionality for reporting buffers, and must be provided by the implementer
	/// in all IR MEL implementations. See individual members for details
	class Buffer
	{
	public:
		/// @Required Implementation required for all IR MEL Implementations
		virtual ~Buffer() = default;
		/**
		 * @brief This operation initializes the Buffer object.
		 *
		 * @param addr the address of the memory location where the buffer begins
		 * @param sz the size in bytes of the buffer
		 * @param ctx the context of the buffer
		 * @return RETURN_SUCCESS on success
		 */
		/// @Required This function supports initialization of buffer object and must be provided by the implementer
		/// for all IR MEL implementations
		virtual Return init(void* addr, const size_t sz, const int64_t ctx) = 0;
		/**
		 * @brief Retrieves the memory address of the buffer
		 *
		 * @return the starting address of the memory buffer
		 */
		/// @Required This function supports getting memory pointer and must be provided by the implementer for all IR MEL implementations
		[[nodiscard]] virtual void* getBufferAddress() const = 0;
		/**
		 * @brief Retrieves the memory address of the image in the buffer
		 *
		 * @return the starting address of the image in the memory buffer
		 */
		/// @Required This function supports getting memory pointer and must be provided by the implementer for all IR MEL implementations
		[[nodiscard]] virtual void* getImageAddress() const = 0;

		/**
		 * @brief Retrieves the size of the memory associated with the buffer.
		 *
		 * @return the size of the buffer in bytes
		 */
		/// @Required This function supports getting buffer size and must be provided by the implementer for all IR MEL implementations
		[[nodiscard]] virtual int64_t getSize() const = 0;
		/**
		 * @brief Retrieves the user-specified context associated with the buffer during initialization
		 *
		 * @return the address of the user-specified context associated with the buffer
		 */
		/// @Required This function supports getting user context and must be provided by the implementer for all IR MEL implementations
		[[nodiscard]] virtual int64_t getContext() const = 0;
		/**
		 * @brief After a buffer handle is provided via the image produced callback it must be released via release() operation to allow the
		 * library to reuse the buffer for a new image.
		 */
		/// @Required This function supports the callback release and must be provided by the implementer for all IR MEL implementations
		virtual Return release() = 0;
		/**
		 * @brief Gets all the flags associated with the buffer
		 *
		 * @param [out] flags The list of flags associated with the buffer
		 *
		 * @return RETURN_SUCCESS on success
		 */
		/// @Required This function supports getting flags and must be provided by the implementer for all IR MEL implementations
		virtual Return getFlags(std::vector<BufferFlag>& flags) const = 0;
		/**
		 * @brief Adds a single flag associated with the buffer
		 *
		 * @param flag The flag associated with the buffer
		 */
		/// @Required This function supports adding a flag and must be provided by the implementer for all IR MEL implementations
		virtual void addFlag(BufferFlag flag) = 0;
		/**
		 * @brief Sets all the flags associated with the buffer
		 *
		 * @param flags The list of flags associated with the buffer
		 */
		/// @Required This function supports setting flags and must be provided by the implementer for all IR MEL implementations
		virtual void setFlags(const std::vector<BufferFlag>& flags) = 0;

		Buffer() = default;
		Buffer(const Buffer&) = default;
		Buffer& operator=(const Buffer&) = default;
		Buffer(Buffer&& other) = default;
		Buffer& operator=(Buffer&& other) = default;
	};

// extern "C" of a function that returns a shared_ptr is not supported by clang as it
//    notes that if C called this function, it would have trouble converting a shared_ptr
//    to C as there is no direct translation. This warning is being ignored as this function
//    is not being called by C, but requires C-Linkage for dlopen and dlsym calls as a result
//    of the dynamic library loading in LibraryLoader.cpp
#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
	extern "C" IR_MEL_EXPORT std::shared_ptr<Buffer> getBuffer(std::string_view instance, std::shared_ptr<::API_Manager> sam);
#if __clang__
#pragma clang diagnostic pop
#endif
} // end namespace ams::iface::irmel
