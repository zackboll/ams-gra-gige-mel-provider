#include <irmel/library/irmel-types/Control.h>

#include <iostream>

int main() {
    auto manager = getAPI_Manager("synthetic");
    auto control = ams::iface::irmel::getControl("synthetic", manager);
    if (!manager || !control || control->init() != ams::iface::irmel::Return::Success ||
        control->getCapabilities().size() != 2) {
        std::cerr << "Installed package did not expose the expected provider\n";
        return 1;
    }
    std::cout << "PASS: installed headers, CMake package, shared library, and factories\n";
}
