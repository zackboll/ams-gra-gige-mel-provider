#pragma once

#include "mel_types.hpp"
#include <mutex>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class HostBuffer final : public ir::Buffer {
public:
    explicit HostBuffer(std::shared_ptr<const int> affinity);

    ir::Return init(void* address, std::size_t size, std::int64_t context) override;
    void* getBufferAddress() const override;
    void* getImageAddress() const override;
    std::int64_t getSize() const override;
    std::int64_t getContext() const override;

    ir::Return release() override;
    ir::Return getFlags(std::vector<ir::BufferFlag>& flags) const override;
    void addFlag(ir::BufferFlag flag) override;
    void setFlags(const std::vector<ir::BufferFlag>& flags) override;

    ir::Return bind(const void* owner, const std::shared_ptr<const int>& affinity, std::size_t minimum_size);
    void unbind();
    bool outstanding() const;
    bool reserved() const;
    bool overlaps_reserved(std::uintptr_t start, std::uintptr_t end) const;
    bool acquire(std::size_t minimum_size);
    void publish();
    void cancel_fill();

private:
    enum class State { available, filling, delivered };
    std::shared_ptr<const int> affinity_;
    mutable std::mutex mutex_;
    void* address_{nullptr};
    std::size_t size_{0};
    std::int64_t context_{0};
    const void* owner_{nullptr};
    State state_{State::available};
    std::vector<ir::BufferFlag> flags_{ir::BufferFlag::Host};
};

} // namespace provider_detail
} // namespace gige_mel
