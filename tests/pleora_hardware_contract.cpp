// EXPLICIT OPT-IN HARDWARE TEST. Runs only for a caller-selected exact MAC.
// It starts/stops acquisition at the camera's CURRENT admitted configuration;
// it never normalizes format/trigger/auto/timing/ROI. Native acquisition does
// negotiate packet size, set the stream destination and issue start/stop.
// Environment/device/receive failures FAIL this test; none are skips.
//
// Production timestamps remain unqualified. Positive MEL publication uses
// the existing SDK-free test-only clock decorator (historical Aravis name),
// and virtual optics. This proves pixel/ownership integration under TEST TIME,
// not camera exposure timing, PTP/UTC, or real optical calibration.
#include "pleora_backend.hpp"
#include "pleora_private.hpp"
#include "aravis_virtual_clock.hpp"
#ifdef GIGE_MEL_PLEORA_CONTROLLED_MEL
#include "backends/pleora/pleora_features.hpp"
#include "pleora_controlled_backend.hpp"
#endif
#include <gige_mel/provider.hpp>

#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace backend = gige_mel::backend;
namespace pleora = backend::pleora;
namespace virtual_clock = aravis_virtual_clock;
namespace ir = ams::iface::irmel;
using namespace std::chrono_literals;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression))                                                                           \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +          \
                                     ": " #expression);                                             \
    } while (false)

constexpr auto frame_bound = 10s;

template <class T> T take(backend::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

void checked(backend::Status status) {
    if (!status) throw std::runtime_error(status.error().message);
}

template <class Predicate> void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + frame_bound;
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}

bool unqualified(const backend::FrameTimestamp& timestamp) {
    return !timestamp.valid && timestamp.value == 0ns && timestamp.clock_generation == 0 &&
           timestamp.domain == backend::ClockDomain::unspecified &&
           timestamp.event == backend::TimestampEvent::unspecified;
}

bool same_except_timestamp(const backend::FrameDescriptor& a, const backend::FrameDescriptor& b) {
    return a.width == b.width && a.height == b.height && a.row_stride_bytes == b.row_stride_bytes &&
           a.storage_bits_per_pixel == b.storage_bits_per_pixel && a.significant_bits_per_pixel == b.significant_bits_per_pixel &&
           a.bands == b.bands && a.pixel_format == b.pixel_format && a.byte_order == b.byte_order && a.status == b.status &&
           a.native_frame_id == b.native_frame_id && a.stream_generation == b.stream_generation &&
           a.configuration_generation == b.configuration_generation && a.configuration == b.configuration;
}

void source_descriptor(const backend::FrameDescriptor& descriptor, std::size_t bytes,
                       const backend::CameraConfiguration& configuration, backend::PixelFormat format) {
    CHECK(descriptor.configuration == configuration && descriptor.width == configuration.width &&
          descriptor.height == configuration.height && descriptor.pixel_format == format);
    const auto layout = backend::image_layout(format, configuration.width, configuration.height);
    CHECK(layout && descriptor.row_stride_bytes >= layout->row_bytes);
    CHECK(descriptor.storage_bits_per_pixel == layout->storage_bits &&
          descriptor.significant_bits_per_pixel == layout->significant_bits && descriptor.byte_order == layout->byte_order);
    CHECK(descriptor.bands == 1 && descriptor.status == backend::FrameStatus::complete);
    CHECK(descriptor.stream_generation > 0 && descriptor.configuration_generation > 0);
    CHECK(bytes >= (static_cast<std::size_t>(configuration.height) - 1) * descriptor.row_stride_bytes + layout->row_bytes);
    CHECK(unqualified(descriptor.timestamp));
}

backend::FrameLease next_frame(backend::IFrameStream& stream) { return take(stream.receive(frame_bound)); }

struct NativeSession {
    std::shared_ptr<backend::ICameraDevice> device;
    std::unique_ptr<backend::IFrameStream> stream;
    ~NativeSession() {
        if (stream) stream->stop();
        if (device) { try { (void)device->close(); } catch (...) {} }
    }
};

struct CameraFacts {
    backend::CameraConfiguration configuration;
    backend::PixelFormat format;
};

[[maybe_unused]] CameraFacts native_pool_stop_close_and_reopen(const std::shared_ptr<backend::ICameraBackend>& camera,
                                              const std::string& device_id) {
    const auto discovered = take(camera->enumerate());
    CHECK(std::count_if(discovered.begin(), discovered.end(), [&](const auto& info) {
        return info.backend_id == "pleora" && info.device_id == device_id;
    }) == 1);
    std::vector<std::optional<backend::FrameLease>> held;
    std::vector<std::vector<std::byte>> copies;
    NativeSession session;
    session.device = take(camera->open(device_id));
    const CameraFacts facts{take(session.device->configuration()), session.device->capabilities().pixel_format};
    std::cout << "Current admitted configuration: " << facts.configuration.width << 'x' << facts.configuration.height
              << " offset " << facts.configuration.offset_x << ',' << facts.configuration.offset_y
              << ", " << facts.configuration.frame_rate_hz << " Hz, exposure " << facts.configuration.exposure.count()
              << " ns, " << (facts.format == backend::PixelFormat::mono8 ? "Mono8" : "Mono12") << '\n';
    auto invalid = facts.configuration;
    invalid.width = 0;
    const auto refused = session.device->apply(invalid);
    CHECK(!refused && refused.error().code == backend::ErrorCode::invalid_argument);
    CHECK(take(session.device->configuration()) == facts.configuration);
    session.stream = take(session.device->create_stream());
    checked(session.stream->start(101));
    std::set<const std::byte*> addresses;
    for (std::size_t i = 0; i < 4; ++i) {
        held.emplace_back(next_frame(*session.stream));
        const auto& frame = *held.back();
        source_descriptor(frame.descriptor(), frame.bytes().size(), facts.configuration, facts.format);
        CHECK(frame.descriptor().configuration_generation == 101);
        CHECK(addresses.insert(frame.bytes().data()).second);
        copies.emplace_back(frame.bytes().begin(), frame.bytes().end());
    }
    const auto first_generation = held.front()->descriptor().stream_generation;
    const auto full = session.stream->receive(150ms);
    CHECK(!full && full.error().code == backend::ErrorCode::resource_exhausted);
    CHECK(session.stream->statistics().pool_starvation_events >= 1);
    for (std::size_t i = 0; i < held.size(); ++i)
        CHECK(std::equal(copies[i].begin(), copies[i].end(), held[i]->bytes().begin(), held[i]->bytes().end()));

    // Releasing one host-copy lease makes exactly that host slot reusable.
    const auto* reusable = held.front()->bytes().data();
    held.front().reset();
    held.front().emplace(next_frame(*session.stream));
    CHECK(held.front()->bytes().data() == reusable);
    copies.front().assign(held.front()->bytes().begin(), held.front()->bytes().end());
    for (std::size_t i = 1; i < held.size(); ++i)
        CHECK(std::equal(copies[i].begin(), copies[i].end(), held[i]->bytes().begin(), held[i]->bytes().end()));

    // Unlike PR #25's native pool, a full copy pool does not block reception.
    // Deterministic blocked-native-poll cancellation is covered offline instead.
    session.stream->request_stop();
    const auto cancelled = session.stream->receive(30s);
    CHECK(!cancelled && cancelled.error().code == backend::ErrorCode::stopped);
    session.stream->stop();
    session.stream->stop();
    const auto restart_held = session.stream->start(102);
    CHECK(!restart_held && restart_held.error().code == backend::ErrorCode::resource_exhausted);
    checked(session.device->close());
    CHECK(!session.device->configuration());
    session.stream.reset();
    session.device.reset();
    for (std::size_t i = 0; i < held.size(); ++i)
        CHECK(std::equal(copies[i].begin(), copies[i].end(), held[i]->bytes().begin(), held[i]->bytes().end()));
    // Old source leases deliberately outlive both the stream/device objects
    // and a distinct, reopened run.
    auto reopened = take(camera->open(device_id));
    CHECK(take(reopened->configuration()) == facts.configuration);
    auto restarted = take(reopened->create_stream());
    checked(restarted->start(202));
    {
        const auto frame = next_frame(*restarted);
        source_descriptor(frame.descriptor(), frame.bytes().size(), facts.configuration, facts.format);
        CHECK(frame.descriptor().configuration_generation == 202);
    }
    restarted->stop();
    checked(reopened->close());
    for (std::size_t i = 0; i < held.size(); ++i)
        CHECK(std::equal(copies[i].begin(), copies[i].end(), held[i]->bytes().begin(), held[i]->bytes().end()));
    held.clear();
    CHECK(first_generation > 0); // stream generations are local to each stream
    std::cout << "PASS native descriptor, four-buffer pressure/release, cancellation, retained bytes, explicit reopen\n";
    return facts;
}

struct Delivery {
    ir::FrameHeader header;
    virtual_clock::SourceFrame source;
    std::vector<std::byte> pixels;
    bool matches_source{false};
};

class Listener final : public ir::ImageListener {
public:
    explicit Listener(std::shared_ptr<virtual_clock::Ledger> observed) : ledger(std::move(observed)) {}
    void onImage(const ir::Channel&, const ir::FrameHeader& header, std::shared_ptr<ir::Buffer> buffer) override {
        Delivery delivery;
        delivery.header = header;
        const auto sources = ledger->all();
        const auto source = std::find_if(sources.rbegin(), sources.rend(), [&](const auto& item) {
            return static_cast<std::uint32_t>(item.original.native_frame_id & 0xffffffffULL) == header.getFrameID();
        });
        if (!buffer || source == sources.rend()) throw std::runtime_error("MEL callback has no original SDK frame");
        delivery.source = *source;
        const auto& original = source->original;
        const auto layout = backend::image_layout(original.pixel_format, original.width, original.height);
        if (!layout) throw std::runtime_error("invalid source layout in MEL callback");
        const auto* destination = static_cast<const std::byte*>(buffer->getImageAddress());
        delivery.pixels.assign(destination, destination + layout->image_bytes);
        delivery.matches_source = ledger->outstanding() > 0 && source->original_data == source->delivered_data &&
                                  source->original_size == source->delivered_size;
        for (std::uint32_t row = 0; row < original.height; ++row) {
            const auto* start = source->original_data + static_cast<std::size_t>(row) * original.row_stride_bytes;
            delivery.matches_source = delivery.matches_source &&
                std::equal(start, start + layout->row_bytes, destination + static_cast<std::size_t>(row) * layout->row_bytes);
        }
        {
            std::lock_guard lock(mutex);
            if (deliveries.empty()) retained = buffer; // consumer keeps first buffer through disable
            else CHECK(buffer->release() == ir::Return::Success);
            deliveries.push_back(std::move(delivery));
        }
        changed.notify_all();
    }
    void wait(std::size_t count) {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, frame_bound, [&] { return deliveries.size() >= count; }));
    }
    std::vector<Delivery> all() const { std::lock_guard lock(mutex); return deliveries; }
    void release_retained() {
        std::lock_guard lock(mutex);
        if (!retained) return;
        const auto* bytes = static_cast<const std::byte*>(retained->getImageAddress());
        CHECK(std::equal(deliveries.front().pixels.begin(), deliveries.front().pixels.end(), bytes));
        CHECK(retained->release() == ir::Return::Success);
        retained.reset();
    }

private:
    std::shared_ptr<virtual_clock::Ledger> ledger;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<Delivery> deliveries;
    std::shared_ptr<ir::Buffer> retained;
};

void mel_admission(const std::string& device_id, const CameraFacts& facts, bool test_clock,
                   std::shared_ptr<backend::ICameraBackend> camera = pleora::make_backend()) {
    // Caller memory and listener outlive every callback and the provider.
    std::deque<std::vector<std::byte>> storage;
    auto ledger = std::make_shared<virtual_clock::Ledger>();
    auto listener = std::make_shared<Listener>(ledger);
    std::shared_ptr<virtual_clock::Clock> clock;
    virtual_clock::Model model;
    if (test_clock) {
        clock = std::make_shared<virtual_clock::Clock>();
        model.epoch = 1000s;
        model.period = 1s; // declared virtual progression; no camera-time inference
        model.clock_generation = 26;
        clock->set_model(model);
    }
    auto decorated = std::make_shared<virtual_clock::Backend>(std::move(camera), clock, ledger);
    gige_mel::ProviderSettings settings;
    settings.instance = "pleora-explicit-hardware-contract";
    settings.device_id = device_id;
    settings.maximum_registered_buffers = 4;
    settings.horizontal_radians_per_pixel = 0.0002; // VIRTUAL test optics
    settings.vertical_radians_per_pixel = 0.0003;
    settings.timestamp_domain = backend::ClockDomain::device;
    auto manager = gige_mel::make_api_manager(settings, decorated);
    auto control = ir::getControl(settings.instance, manager);
    CHECK(control && control->init() == ir::Return::Success);
    const auto capabilities = control->getCapabilities();
    const auto found = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& cap) {
        return cap.getChannelTypes() == std::vector{ir::ChannelType::IRSTImage};
    });
    CHECK(found != capabilities.end());
    ir::Config selected;
    selected.setChanID(found->getChanID());
    selected.setChannelType(ir::ChannelType::IRSTImage);
    selected.setImgLstnr(listener);
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(selected));
    CHECK(image);
    std::vector<std::shared_ptr<ir::Buffer>> buffers;
    struct Cleanup {
        std::shared_ptr<ir::Control> control;
        std::shared_ptr<ir::ImageChannel> image;
        std::vector<std::shared_ptr<ir::Buffer>>& buffers;
        bool active{true};
        ~Cleanup() {
            if (!active) return;
            try { (void)image->disable(); } catch (...) {}
            for (const auto& buffer : buffers) {
                try { (void)buffer->release(); (void)image->unregisterBuffer(buffer); } catch (...) {}
            }
            try { (void)control->detachChannel(image); } catch (...) {}
        }
    } cleanup{control, image, buffers};
    const auto layout = backend::image_layout(facts.format, facts.configuration.width, facts.configuration.height);
    CHECK(layout && static_cast<std::size_t>(image->getCapabilities().getBufferSize()) == layout->image_bytes);
    for (std::int64_t i = 0; i < 4; ++i) {
        storage.emplace_back(layout->image_bytes, std::byte{0xa5});
        auto buffer = ir::getBuffer(settings.instance, manager);
        CHECK(buffer && buffer->init(storage.back().data(), storage.back().size(), i) == ir::Return::Success);
        CHECK(image->registerBuffer(buffer) == ir::Return::Success);
        buffers.push_back(std::move(buffer));
    }
    CHECK(image->enable() == ir::Return::Success);
    if (test_clock) listener->wait(3);
    else {
        if (!ledger->wait_frames(3, frame_bound))
            throw std::runtime_error("source did not deliver: " + gige_mel::provider_statistics(manager).last_error);
        until([&] { return gige_mel::provider_statistics(manager).rejected_frames >= 3; });
    }
    CHECK(image->disable() == ir::Return::Success);
    CHECK(ledger->wait_outstanding(0, frame_bound));
    const auto stats = gige_mel::provider_statistics(manager);
    CHECK(stats.stream_failures == 0 && stats.callback_failures == 0);
    const auto delivered = listener->all();
    if (!test_clock) {
        CHECK(delivered.empty() && stats.delivered_frames == 0 && stats.rejected_frames >= 3);
        CHECK(stats.last_error.find("timestamp contract") != std::string::npos);
        for (const auto& source : ledger->all()) {
            source_descriptor(source.original, source.original_size, facts.configuration, facts.format);
            CHECK(!source.substituted && unqualified(source.delivered.timestamp));
        }
        for (const auto& bytes : storage)
            CHECK(std::all_of(bytes.begin(), bytes.end(), [](std::byte value) { return value == std::byte{0xa5}; }));
        std::cout << "PASS MEL rejects native unqualified timestamps; zero Image callbacks\n";
    } else {
        CHECK(delivered.size() >= 3 && stats.delivered_frames == delivered.size() && stats.rejected_frames == 0);
        for (const auto& item : delivered) {
            const auto& source = item.source;
            source_descriptor(source.original, source.original_size, facts.configuration, facts.format);
            CHECK(item.matches_source && source.substituted && same_except_timestamp(source.original, source.delivered));
            const auto declared = virtual_clock::nominal(model, source.sequence);
            CHECK(declared && source.delivered.timestamp.value == declared->value &&
                  source.delivered.timestamp.clock_generation == model.clock_generation);
            const auto& header = item.header;
            CHECK(header.getSystemTime() == declared->value && header.getIntegrationTime() == facts.configuration.exposure);
            CHECK(header.getWidth() == facts.configuration.width && header.getHeight() == facts.configuration.height);
            CHECK(header.getColumnOffset() == facts.configuration.offset_x && header.getRowOffset() == facts.configuration.offset_y);
            CHECK(header.getBitsPerPixel() == layout->storage_bits && header.getNumBands() == 1 && header.getFormat() == ir::PixelFormat::Mono);
            CHECK(header.getFrameID() == static_cast<std::uint32_t>(source.original.native_frame_id & 0xffffffffULL));
            CHECK(header.getSubframeID() == 0 && header.getSubframeTotal() == 1 && header.getImageType() == ir::ImageType::Staring);
            CHECK(item.pixels.size() == layout->image_bytes);
#ifdef GIGE_MEL_PLEORA_CONTROLLED_MEL
            // Independent expected pattern detects a wrong copy even if the
            // source/destination comparison accidentally shared an address.
            for (std::size_t i = 0; i < 128; ++i) {
                const auto expected = (i + source.original.native_frame_id) &
                    (facts.format == backend::PixelFormat::mono8 ? 255u : 4095u);
                CHECK(item.pixels[i * (layout->storage_bits / 8)] == static_cast<std::byte>(expected & 255u));
                if (facts.format == backend::PixelFormat::mono12)
                    CHECK(item.pixels[2*i+1] == static_cast<std::byte>(expected >> 8));
            }
#endif
        }
        std::cout << "PASS MEL copies actual SDK pixels with isolated TEST clock/optics; camera time remains unqualified\n";
    }
    // Explicit teardown before caller memory disappears; no callbacks after disable.
    const auto count = listener->all().size();
    listener->release_retained();
    const auto prior_frames = ledger->frames();
    CHECK(image->enable() == ir::Return::Success);
    CHECK(ledger->wait_frames(prior_frames + 3, frame_bound));
    CHECK(image->disable() == ir::Return::Success);
    CHECK(ledger->wait_outstanding(0, frame_bound));
    const auto final_count = listener->all().size();
    CHECK(test_clock ? final_count > count : final_count == 0);
    for (const auto& buffer : buffers) {
        // Callback already released delivered buffers; rejected frames never
        // acquired caller buffers. A second release is intentionally invalid.
        CHECK(image->unregisterBuffer(buffer) == ir::Return::Success);
    }
    buffers.clear();
    CHECK(control->detachChannel(image) == ir::Return::Success);
    cleanup.active = false;
    cleanup.image.reset(); cleanup.control.reset();
    image.reset(); control.reset(); manager.reset(); decorated.reset();
    CHECK(ledger->outstanding() == 0 && ledger->released() == ledger->frames());
    CHECK(listener->all().size() == final_count);
}

} // namespace

int main(int argc, char** argv) {
    try {
#ifdef GIGE_MEL_PLEORA_CONTROLLED_MEL
        (void)argc; (void)argv;
        for (auto format : {backend::PixelFormat::mono8, backend::PixelFormat::mono12}) {
            const CameraFacts facts{pleora_controlled::configuration, format};
            for (bool test_clock : {false, true})
                mel_admission("controlled-not-camera", facts, test_clock,
                    std::make_shared<pleora_controlled::Backend>(format));
        }
        std::cout << "PASS controlled SDK-memory MEL integration; NO hardware/time/license qualification\n";
#else
        if (argc != 2) throw std::runtime_error("usage: pleora_hardware_contract aa:bb:cc:dd:ee:ff (explicit licensed physical camera; no evaluation)");
        const std::string device_id = argv[1];
        std::cout << "EXPLICIT HARDWARE CONTRACT " << device_id << "; compiled eBUS " << pleora::compiled_version()
                  << "; runtime " << pleora::runtime_version() << '\n';
        auto camera = pleora::make_backend();
        const auto facts = native_pool_stop_close_and_reopen(camera, device_id);
        mel_admission(device_id, facts, false);
        mel_admission(device_id, facts, true);
        std::cout << "PASS selected-device stream and MEL integration contract; no physical timestamp qualification\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Pleora hardware contract FAILED (not skipped): " << error.what() << '\n';
        return 1;
    }
}
