#include "host_buffer.hpp"
#include <algorithm>
#include <limits>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

HostBuffer::HostBuffer(std::shared_ptr<const int> affinity) : affinity_(std::move(affinity)) {}

ir::Return HostBuffer::init(void* address, std::size_t size, std::int64_t context) {
    std::lock_guard lock(mutex_);
    if (owner_ || state_ != State::available) return ir::Return::Fail;
    if (!address || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        return ir::Return::BadPointer;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin) return ir::Return::BadPointer;
    address_ = address;
    size_ = size;
    context_ = context;
    return ir::Return::Success;
}

void* HostBuffer::getBufferAddress() const { std::lock_guard lock(mutex_); return address_; }

void* HostBuffer::getImageAddress() const { return getBufferAddress(); }

std::int64_t HostBuffer::getSize() const { std::lock_guard lock(mutex_); return static_cast<std::int64_t>(size_); }

std::int64_t HostBuffer::getContext() const { std::lock_guard lock(mutex_); return context_; }

ir::Return HostBuffer::release() {
    std::lock_guard lock(mutex_);
    if (state_ != State::delivered) return ir::Return::Fail;
    state_ = State::available;
    return ir::Return::Success;
}

ir::Return HostBuffer::getFlags(std::vector<ir::BufferFlag>& flags) const {
    std::lock_guard lock(mutex_);
    flags = flags_;
    return ir::Return::Success;
}

void HostBuffer::addFlag(ir::BufferFlag flag) {
    std::lock_guard lock(mutex_);
    if (!owner_ && state_ == State::available && std::find(flags_.begin(), flags_.end(), flag) == flags_.end())
        flags_.push_back(flag);
}

void HostBuffer::setFlags(const std::vector<ir::BufferFlag>& flags) {
    std::lock_guard lock(mutex_);
    if (!owner_ && state_ == State::available) flags_ = flags;
}

ir::Return HostBuffer::bind(const void* owner, const std::shared_ptr<const int>& affinity, std::size_t minimum_size) {
    std::lock_guard lock(mutex_);
    if (affinity != affinity_) return ir::Return::BadPointer;
    if (!address_ || size_ < minimum_size) return ir::Return::BadPointer;
    if (owner_ || state_ != State::available) return ir::Return::Fail;
    if (flags_.size() != 1 || flags_[0] != ir::BufferFlag::Host) return ir::Return::NotSupported;
    owner_ = owner;
    return ir::Return::Success;
}

void HostBuffer::unbind() { std::lock_guard lock(mutex_); owner_ = nullptr; }

bool HostBuffer::outstanding() const { std::lock_guard lock(mutex_); return state_ != State::available; }

bool HostBuffer::reserved() const { std::lock_guard lock(mutex_); return owner_ || state_ != State::available; }

bool HostBuffer::overlaps_reserved(std::uintptr_t start, std::uintptr_t end) const {
    std::lock_guard lock(mutex_);
    if (!owner_ && state_ == State::available) return false;
    const auto other = reinterpret_cast<std::uintptr_t>(address_);
    return start < other + size_ && other < end;
}

bool HostBuffer::acquire(std::size_t minimum_size) {
    std::lock_guard lock(mutex_);
    if (!owner_ || state_ != State::available || size_ < minimum_size) return false;
    state_ = State::filling;
    return true;
}

void HostBuffer::publish() { std::lock_guard lock(mutex_); state_ = State::delivered; }

void HostBuffer::cancel_fill() { std::lock_guard lock(mutex_); if (state_ == State::filling) state_ = State::available; }

} // namespace provider_detail
} // namespace gige_mel
