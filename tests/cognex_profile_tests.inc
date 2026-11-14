// Typed fixtures reproduce documented values, not physical qualification.
RecordingFeatures cognex_features() {
    auto f = baseline();
    f.nodes["DeviceVendorName"] = text_node("COGNEX", Kind::string);
    f.nodes["DeviceModelName"] = text_node("CIC-5000R", Kind::string);
    f.nodes["DeviceFirmwareVersion"] = text_node("106349-08;U;CIC-5000R;V3.7-1;0", Kind::string);
    for (auto name : {"DeviceVendorName", "DeviceModelName", "DeviceFirmwareVersion"})
        f.nodes[name].access = Access::read_only;
    f.nodes["SensorWidth"] = integer_node(2592, 2592, 2592, 1, Access::read_only);
    f.nodes["SensorHeight"] = integer_node(1944, 1944, 1944, 1, Access::read_only);
    f.nodes["Width"] = integer_node(2592, 64, 2592);
    f.nodes["Height"] = integer_node(1944, 64, 1944);
    f.nodes["OffsetX"] = integer_node(0, 0, 0);
    f.nodes["OffsetY"] = integer_node(0, 0, 0);
    f.nodes.erase("AcquisitionFrameRate");
    f.nodes["AcquisitionFrameRateAbs"] = float_node(10, 0.0149, 1'000'000, "Hz");
    f.nodes["ExposureTimeAbs"] = float_node(35'000, 35, 999'985, "us");
    f.nodes["AcquisitionFrameRateEnable"] = boolean_node(true); // explicit fixture arrangement, not power-on state
    f.nodes["AcquisitionMode"] = text_node("Continuous");
    f.nodes["ExposureAuto"] = text_node("Off");
    return f;
}

void cognex_contract() {
    namespace s = gige_mel::selection;
    namespace p = backend::profiles;
    const auto take = []<class T>(backend::Result<T> result) {
        if (!result) throw std::runtime_error(result.error().message);
        return std::move(result).value();
    };
    const auto mapped = [] {
        void* handle = dlopen(GIGE_MEL_COGNEX_MODULE, RTLD_NOW | RTLD_NOLOAD);
        if (handle) dlclose(handle);
        return handle != nullptr;
    };
    CHECK(!mapped());
    for (int session = 0; session < 20; ++session) {
        auto f = cognex_features();
        std::shared_ptr<const p::ICameraProfileState> state;
        {
            auto loaded = take(s::load_profile_module(GIGE_MEL_COGNEX_MODULE));
            CHECK(loaded.profile->id() == "cognex_cic_5000r_v1");
            for (auto name : {"DeviceVendorName", "DeviceModelName", "DeviceFirmwareVersion"}) {
                for (int failure = 0; failure < 4; ++failure) {
                    auto wrong = cognex_features();
                    if (failure == 0) wrong.nodes[name].text += "-different";
                    if (failure == 1) wrong.nodes.erase(name);
                    if (failure == 2) wrong.nodes[name].access = Access::none;
                    if (failure == 3) wrong.on_call = [name](auto&, const Call& c) -> std::optional<backend::Error> {
                        if (c.feature == name) return backend::Error{backend::ErrorCode::io_error, "native identity failure"};
                        return {};
                    };
                    auto rejected = loaded.profile->open(wrong);
                    CHECK(!rejected && contains(rejected.error().message, name));
                    CHECK(wrong.writes().empty());
                    CHECK(wrong.count("read_integer", "SensorWidth") == 0); // identity boundary comes first
                    if (failure == 3) CHECK(contains(rejected.error().message, "native identity failure"));
                }
            }
            auto power_on = cognex_features();
            power_on.nodes["AcquisitionFrameRateEnable"].boolean = false;
            expect_error(loaded.profile->open(power_on), backend::ErrorCode::unsupported, "AcquisitionFrameRateEnable");
            CHECK(power_on.writes().empty());
            for (auto name : {"AcquisitionFrameRateEnable", "TriggerMode", "AcquisitionMode"}) {
                auto absent = cognex_features();
                absent.nodes.erase(name);
                auto failed = loaded.profile->open(absent);
                CHECK(!failed && contains(failed.error().message, name) && absent.writes().empty());
            }
            auto mono12 = cognex_features();
            mono12.nodes["PixelFormat"] = text_node("Mono12");
            mono12.nodes["DeviceStreamChannelEndianness"] = text_node("Little");
            expect_error(loaded.profile->open(mono12), backend::ErrorCode::unsupported, "Mono8");
            state = take(loaded.profile->open(f));
            auto independent = take(loaded.profile->open(f));
            CHECK(independent != state);
            auto narrow = cognex_features();
            narrow.nodes["AcquisitionFrameRateAbs"].real.minimum = 3;
            narrow.nodes["AcquisitionFrameRateAbs"].real.maximum = 9;
            auto restricted = take(loaded.profile->open(narrow));
            CHECK(restricted->capabilities().minimum_frame_rate_hz == 4);
            CHECK(restricted->capabilities().maximum_frame_rate_hz == 8);
            narrow.nodes["AcquisitionFrameRateAbs"].real.minimum = 11;
            narrow.nodes["AcquisitionFrameRateAbs"].real.maximum = 16;
            CHECK(!loaded.profile->open(narrow));
        } // profile retired before state use
        CHECK(mapped());
        CHECK(state->capabilities().maximum_frame_rate_hz == 10);
        CHECK(state->capabilities().sensor_width == 2592 && state->capabilities().sensor_height == 1944);
        auto request = take(state->configuration(f, connected));
        CHECK(request.exposure == 35ms && request.frame_rate_hz == 10);
        for (auto rate : {1u, 2u, 4u, 5u, 8u, 10u}) {
            request.frame_rate_hz = rate;
            CHECK(take(state->apply(f, request, connected, allow_write)) == request);
            CHECK(take(state->configuration(f, connected)) == request);
            CHECK(take(state->preflight(f)) == request);
        }
        for (auto rate : {0u, 3u, 6u, 7u, 9u, 12u, 14u, 16u, 100u}) {
            request.frame_rate_hz = rate;
            f.calls.clear();
            expect_error(state->apply(f, request, connected, allow_write), backend::ErrorCode::unsupported, "AcquisitionFrameRateAbs");
            CHECK(f.writes().empty());
        }
        f.nodes["AcquisitionFrameRateAbs"].real.value = 16;
        CHECK(take(state->configuration(f, connected)).frame_rate_hz == 16);
        expect_error(state->preflight(f), backend::ErrorCode::unsupported, "AcquisitionFrameRateAbs");
        request.frame_rate_hz = 5;
        CHECK(state->apply(f, request, connected, allow_write)); // explicit recovery
        f.nodes["AcquisitionFrameRateEnable"].boolean = false;
        f.calls.clear();
        expect_error(state->apply(f, request, connected, allow_write), backend::ErrorCode::unsupported, "AcquisitionFrameRateEnable");
        expect_error(state->preflight(f), backend::ErrorCode::unsupported, "AcquisitionFrameRateEnable");
        CHECK(f.writes().empty());
        f.nodes["AcquisitionFrameRateEnable"].boolean = true;
        for (auto name : {"TriggerMode", "AcquisitionMode", "ExposureAuto"}) {
            const auto original = f.nodes[name].text;
            f.nodes[name].text = "unsupported";
            f.calls.clear();
            CHECK(!state->apply(f, request, connected, allow_write));
            CHECK(!state->preflight(f));
            CHECK(f.writes().empty());
            f.nodes[name].text = original;
        }
        f.nodes["AcquisitionFrameRateAbs"].real.value = 14.000112;
        expect_error(state->configuration(f, connected), backend::ErrorCode::unsupported, "14.000112");
        CHECK(!state->preflight(f));
        CHECK(state->apply(f, request, connected, allow_write));
        f.nodes["ExposureTimeAbs"].real.maximum = 34'000;
        f.calls.clear();
        CHECK(!state->apply(f, request, connected, allow_write));
        CHECK(f.writes().empty()); // fresh native bounds, not only open-time facts
        f.nodes["ExposureTimeAbs"].real.maximum = 999'985;
        // Native exposure quantization remains authoritative, not requested state.
        f.after_write = [](auto& device, const Call& c) {
            if (c.feature == "ExposureTimeAbs") device.nodes[c.feature].real.value = 34'965;
        };
        request.exposure = 35ms;
        CHECK(take(state->apply(f, request, connected, allow_write)).exposure == 34'965us);
        f.after_write = {};
        request.frame_rate_hz = 2;
        request.exposure = 36ms;
        f.on_call = [](auto&, const Call& c) -> std::optional<backend::Error> {
            if (c.operation == "write_float" && c.feature == "ExposureTimeAbs")
                return backend::Error{backend::ErrorCode::io_error, "native exposure write failed"};
            return {};
        };
        auto partial = state->apply(f, request, connected, allow_write);
        expect_error(partial, backend::ErrorCode::io_error, "native exposure write failed");
        CHECK(contains(partial.error().message, "earlier successful writes:"));
        CHECK(contains(partial.error().message, "actual native configuration:"));
        CHECK(contains(partial.error().message, "no rollback"));
        auto actual = take(state->configuration(f, connected));
        CHECK(actual.frame_rate_hz == 2 && actual.exposure == 34'965us);
        f.on_call = {};
        CHECK(state->apply(f, request, connected, allow_write));
        p::NativeFrameFacts facts;
        facts.status = p::NativeStatus::success; facts.payload = p::NativePayload::image;
        facts.parts = 1; facts.part_is_2d_image = true; facts.pixel_format = p::pfnc_mono8;
        facts.width = request.width; facts.height = request.height; facts.data_present = true;
        facts.image_bytes = facts.received_bytes = facts.capacity_bytes = 2592 * 1944;
        for (auto id : {1u, 2u, 65535u}) {
            facts.frame_id = id;
            CHECK(state->validate_frame(facts, request, backend::PixelFormat::mono8).verdict == p::FrameVerdict::accept);
        }
        --facts.capacity_bytes;
        CHECK(state->validate_frame(facts, request, backend::PixelFormat::mono8).verdict != p::FrameVerdict::accept);
        CHECK(state->validate_frame(facts, request, backend::PixelFormat::mono12).verdict == p::FrameVerdict::reject);
        f = {}; // state destructor and capabilities cannot depend on borrowed features
        CHECK(!state->native_diagnostics().empty());
        state.reset();
        CHECK(!mapped()); // actual loader mapping retired after the last state
    }
}