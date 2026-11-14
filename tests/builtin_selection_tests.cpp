#include "selection/builtin_backends.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

int main() {
    try {
        auto result = gige_mel::selection::make_builtin_catalog();
        CHECK(result);
        auto& catalog = result.value();
        auto ids = catalog.backend_ids();
        const std::vector<std::pair<std::string, bool>> expected{
            {"synthetic", true}, {"aravis", GIGE_MEL_EXPECT_ARAVIS}, {"pleora", GIGE_MEL_EXPECT_PLEORA}};
        for (const auto& [id, enabled] : expected) {
            CHECK((std::find(ids.begin(), ids.end(), id) != ids.end()) == enabled);
            for (unsigned i = 0; i < 10; ++i) {
                auto selected = catalog.create(id, gige_mel::selection::mono_free_running_v1);
                CHECK(static_cast<bool>(selected) == enabled);
                if (enabled) CHECK(selected.value()->id() == id);
                else CHECK(selected.error().code == gige_mel::backend::ErrorCode::not_found);
            }
        }
        auto loaded = gige_mel::selection::load_profile_module(GIGE_MEL_TEST_PROFILE_MODULE);
        CHECK(loaded);
        auto dynamic_catalog = gige_mel::selection::make_builtin_catalog({loaded.value()});
        CHECK(dynamic_catalog);
        for (const auto& [id, enabled] : expected) {
            auto selected = dynamic_catalog.value().create(id, loaded.value().profile->id());
            CHECK(static_cast<bool>(selected) == (enabled && id != "synthetic"));
        }
        CHECK(!gige_mel::selection::make_builtin_catalog({loaded.value(), loaded.value()}));
        // A valid module promising only Aravis must never gain Pleora support
        // merely because both SDK constructors accept ICameraProfile.
        auto restricted = gige_mel::selection::load_profile_module(
            std::string(GIGE_MEL_TEST_PROFILE_DIR) + "/libinvalid_profile_17.so");
        CHECK(restricted);
        auto restricted_catalog = gige_mel::selection::make_builtin_catalog({restricted.value()});
        CHECK(restricted_catalog);
        CHECK(!restricted_catalog.value().create("pleora", restricted.value().profile->id()));
        std::cout << "Built-in selection passed without discovery or open\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
