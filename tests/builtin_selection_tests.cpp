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
        std::cout << "Built-in selection passed without discovery or open\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
