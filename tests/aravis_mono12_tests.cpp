// Explicit SIMULATED Mono12 qualification. Stock Aravis Fake has no Mono12
// enum or generator. Public DOM APIs add its enum; a public fill-pattern hook
// records real SDK storage. The decoded-facts seam generates pixels after pop
// and supplies byte counts (no public received-size setter exists). The SDK
// never writes pixels concurrently with project readers. This is simulation.
#include "aravis_fake_support.hpp"
#include "aravis_virtual_clock.hpp"
#include <gige_mel/provider.hpp>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace be = gige_mel::backend;
namespace av = be::aravis;
namespace p = be::profiles::mono_free_running;
namespace vc = aravis_virtual_clock;
namespace ir = ams::iface::irmel;
using namespace std::chrono_literals;

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x); } while (false)

namespace {
ArvDomNode* add(ArvDevice* device, ArvDomNode* parent, const char* tag,
                const char* name = nullptr, const char* text = nullptr) {
    auto* doc = ARV_DOM_DOCUMENT(arv_device_get_genicam(device));
    auto* node = ARV_DOM_NODE(arv_dom_document_create_element(doc, tag));
    CHECK(arv_dom_node_append_child(parent, node));
    if (name) arv_dom_element_set_attribute(ARV_DOM_ELEMENT(node), "Name", name);
    if (text) CHECK(arv_dom_node_append_child(node, ARV_DOM_NODE(arv_dom_document_create_text_node(doc, text))));
    return node;
}

struct Arrangement {
    std::atomic<bool> fill_ok{true};
    std::atomic<unsigned> writes{0};
    std::mutex mutex;
    std::map<std::uint64_t, unsigned char*> pending;
};

void fill(ArvBuffer* buffer, void* context, guint32, guint32, ArvPixelFormat format) {
    auto& state = *static_cast<Arrangement*>(context);
    std::lock_guard lock(state.mutex);
    const auto w = arv_buffer_get_image_width(buffer), h = arv_buffer_get_image_height(buffer);
    const auto layout = w > 0 && h > 0 ? be::image_layout(be::PixelFormat::mono12,
        static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)) : std::nullopt;
    auto* bytes = static_cast<unsigned char*>(const_cast<void*>(arv_buffer_get_data(buffer, nullptr)));
    if (!layout || !bytes || format != ARV_PIXEL_FORMAT_MONO_12) { state.fill_ok = false; return; }
    const auto id = arv_buffer_get_frame_id(buffer);
    state.pending[id] = bytes;
}

av::TestHooks hooks(Arrangement& state) {
    av::TestHooks result;
    result.after_native_open = [&](ArvDevice* device) {
        auto* enumeration = ARV_DOM_NODE(arv_device_get_feature(device, "PixelFormat"));
        auto* entry = add(device, enumeration, "EnumEntry", "Mono12");
        add(device, entry, "Value", nullptr, "17825797"); // 0x01100005
        auto* root = ARV_DOM_NODE(arv_dom_document_get_document_element(ARV_DOM_DOCUMENT(arv_device_get_genicam(device))));
        auto* endian = add(device, root, "Enumeration", "DeviceStreamChannelEndianness");
        add(device, add(device, endian, "EnumEntry", "Little"), "Value", nullptr, "0");
        add(device, endian, "Value", nullptr, "0");
        GError* error = nullptr;
        arv_device_set_string_feature_value(device, "PixelFormat", "Mono12", &error);
        CHECK(!error);
        arv_device_set_integer_feature_value(device, "Width", 64, &error); CHECK(!error);
        arv_device_set_integer_feature_value(device, "Height", 48, &error); CHECK(!error);
        CHECK(std::string(arv_device_get_string_feature_value(device, "PixelFormat", &error)) == "Mono12");
        CHECK(!error);
        arv_fake_camera_set_fill_pattern(arv_fake_device_get_fake_camera(ARV_FAKE_DEVICE(device)), fill, &state);
    };
    result.before_write = [&](auto, auto*) { ++state.writes; };
    result.after_buffer_allocated = [&](ArvBuffer*) {
        // Publish allocation before the uninstrumented SDK queue transfers it.
        std::lock_guard lock(state.mutex);
    };
    result.decoded_facts = [&](p::NativeFrameFacts& facts) {
        if (facts.status == p::NativeStatus::success && facts.pixel_format == p::pfnc_mono12) {
            std::lock_guard lock(state.mutex);
            const auto layout = be::image_layout(be::PixelFormat::mono12,
                static_cast<std::uint32_t>(facts.width), static_cast<std::uint32_t>(facts.height));
            CHECK(layout && layout->image_bytes <= facts.capacity_bytes);
            auto* bytes = state.pending.at(facts.frame_id);
            state.pending.erase(facts.frame_id);
            for (std::size_t y = 0; y < static_cast<std::size_t>(facts.height); ++y)
                for (std::size_t x = 0; x < static_cast<std::size_t>(facts.width); ++x) {
                    const auto value = (17 * x + 31 * y + facts.frame_id) & 4095;
                    const auto i = y * layout->row_bytes + x * 2;
                    bytes[i] = static_cast<unsigned char>(value & 255);
                    bytes[i + 1] = static_cast<unsigned char>(value >> 8);
                }
            facts.image_bytes = facts.received_bytes = layout->image_bytes;
        }
    };
    return result;
}

void pixels(std::span<const std::byte> bytes, std::uint64_t id) {
    CHECK(bytes.size() == 6144);
    for (std::size_t y = 0; y < 48; ++y) for (std::size_t x = 0; x < 64; ++x) {
        const auto i = (y * 64 + x) * 2;
        CHECK((std::to_integer<unsigned>(bytes[i]) | (std::to_integer<unsigned>(bytes[i + 1]) << 8)) ==
              ((17 * x + 31 * y + id) & 4095));
    }
}

void native_contract() {
    Arrangement state;
    auto camera = aravis_fake::make_backend(hooks(state));
    auto opened = camera->open(aravis_fake::device_id);
    if (!opened) throw std::runtime_error(opened.error().message);
    auto device = opened.value();
    CHECK(device->capabilities().pixel_format == be::PixelFormat::mono12 && !device->capabilities().mono8);
    CHECK(state.writes == 0); // adapter did not write PixelFormat or configuration
    auto stream = device->create_stream().value();
    CHECK(stream->start(22));
    std::vector<be::FrameLease> held;
    for (unsigned i = 0; i < 4; ++i) {
        auto f = stream->receive(1s); CHECK(f);
        held.push_back(std::move(f).value());
        const auto& d = held.back().descriptor();
        CHECK(d.pixel_format == be::PixelFormat::mono12 && d.row_stride_bytes == 128);
        CHECK(d.storage_bits_per_pixel == 16 && d.significant_bits_per_pixel == 12);
        CHECK(d.byte_order == be::ByteOrder::little_endian && !d.timestamp.valid);
        CHECK(d.configuration_generation == 22);
        pixels(held.back().bytes(), d.native_frame_id);
    }
    const auto saved = std::vector(held.front().bytes().begin(), held.front().bytes().end());
    CHECK(stream->receive(30ms).error().code == be::ErrorCode::timeout);
    CHECK(stream->statistics().pool_starvation_events > 0);
    stream->stop();
    CHECK(stream->start(23).error().code == be::ErrorCode::resource_exhausted);
    CHECK(std::equal(saved.begin(), saved.end(), held.front().bytes().begin()));
    const auto old_generation = held.front().descriptor().stream_generation;
    held.clear();
    CHECK(stream->start(23));
    auto fresh = stream->receive(1s).value();
    CHECK(fresh.descriptor().stream_generation != old_generation);
    pixels(fresh.bytes(), fresh.descriptor().native_frame_id);
    stream->stop();
    { auto release = std::move(fresh); }
    CHECK(av::with_native_device_for_testing(*device, [](ArvDevice* d) {
        arv_device_set_string_feature_value(d, "PixelFormat", "Mono8", nullptr);
    }));
    CHECK(!stream->start(24) && !device->configuration());
    CHECK(!device->apply(be::CameraConfiguration{}));
    CHECK(device->close() && state.fill_ok);
    // Diagnostic must never report a truncated Mono8 PGM from Mono12 words.
    const auto capture = av::capture_one(*camera, aravis_fake::device_id, "/dev/null", 1s);
    CHECK(capture.error && capture.error->code == be::ErrorCode::unsupported && !capture.report);
    CHECK(!capture.cleanup_error);
}

void retained_source_teardown() {
    Arrangement state;
    auto camera = aravis_fake::make_backend(hooks(state));
    auto device = camera->open(aravis_fake::device_id).value();
    auto stream = device->create_stream().value();
    CHECK(stream->start(90));
    auto lease = stream->receive(1s).value();
    const auto saved = std::vector(lease.bytes().begin(), lease.bytes().end());
    const auto id = lease.descriptor().native_frame_id;
    stream->request_stop();
    CHECK(device->close());
    stream.reset(); device.reset(); camera.reset();
    CHECK(std::equal(saved.begin(), saved.end(), lease.bytes().begin()));
    pixels(lease.bytes(), id);
    CHECK(state.fill_ok);
    // The final SDK buffer release happens after all adapter objects are gone.
}

class Listener final : public ir::ImageListener {
public:
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count{0};
    bool correct{true};
    void onImage(const ir::Channel&, const ir::FrameHeader& h, std::shared_ptr<ir::Buffer> b) override {
        std::lock_guard lock(mutex);
        try {
            CHECK(h.getBitsPerPixel() == 16 && h.getFormat() == ir::PixelFormat::Mono && h.getNumBands() == 1);
            pixels({static_cast<const std::byte*>(b->getImageAddress()), 6144}, h.getFrameID());
        } catch (...) { correct = false; }
        ++count; changed.notify_all(); // intentionally retain ownership
    }
};

void mel_delivery(bool virtual_time) {
    Arrangement state;
    auto clock = virtual_time ? std::make_shared<vc::Clock>() : nullptr;
    auto decorated = std::make_shared<vc::Backend>(aravis_fake::make_backend(hooks(state)), clock,
                                                   std::make_shared<vc::Ledger>());
    gige_mel::ProviderSettings settings;
    settings.instance = "mono12-simulation"; settings.device_id = std::string(aravis_fake::device_id);
    settings.timestamp_domain = be::ClockDomain::device;
    auto manager = gige_mel::make_api_manager(settings, decorated);
    auto control = ir::getControl(settings.instance, manager);
    CHECK(control->init() == ir::Return::Success);
    auto listener = std::make_shared<Listener>();
    ir::Config config;
    for (const auto& cap : control->getCapabilities()) if (cap.getChannelTypes() == std::vector{ir::ChannelType::IRSTImage}) {
        CHECK(cap.getBitDepth() == 12 && cap.getRowPitch() == 128 && cap.getBufferSize() == 6144);
        config.setChanID(cap.getChanID());
    }
    config.setChannelType(ir::ChannelType::IRSTImage); config.setImgLstnr(listener);
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config)); CHECK(image);
    std::vector<std::byte> storage(6144, std::byte{0x5a});
    auto buffer = ir::getBuffer(settings.instance, manager);
    CHECK(buffer->init(storage.data(), storage.size(), 1) == ir::Return::Success);
    CHECK(image->registerBuffer(buffer) == ir::Return::Success);
    CHECK(image->enable() == ir::Return::Success);
    if (virtual_time) {
        std::unique_lock lock(listener->mutex);
        CHECK(listener->changed.wait_for(lock, 2s, [&] { return listener->count > 0; }));
        CHECK(listener->correct);
    } else {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (gige_mel::provider_statistics(manager).rejected_frames < 2) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
    }
    CHECK(image->disable() == ir::Return::Success);
    if (!virtual_time) {
        CHECK(listener->count == 0 && std::all_of(storage.begin(), storage.end(), [](auto b) { return b == std::byte{0x5a}; }));
    }
    const auto saved = storage;
    CHECK(control->detachChannel(image) == ir::Return::Success);
    control.reset(); manager.reset();
    CHECK(storage == saved && state.fill_ok);
    if (virtual_time) CHECK(buffer->release() == ir::Return::Success);
}
} // namespace

int main() {
    try {
        native_contract(); retained_source_teardown(); mel_delivery(true); mel_delivery(false);
        std::cout << "PASS simulated Mono12: exact pixels, native pool/restart, format-change rejection, MEL delivery, production timestamp rejection, negative controls\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
