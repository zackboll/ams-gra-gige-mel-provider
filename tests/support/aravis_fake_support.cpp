#include "aravis_fake_support.hpp"

#include <dlfcn.h>

#include <string>
#include <utility>

namespace aravis_fake {

std::shared_ptr<gige_mel::backend::ICameraBackend> make_backend(gige_mel::backend::aravis::TestHooks hooks) {
    // arv_fake_interface_get_instance() does not touch the process-wide
    // interface enable flags used by arv_update_device_list().
    return gige_mel::backend::aravis::make_backend_for_interface({"Fake", &arv_fake_interface_get_instance},
                                                                 std::move(hooks));
}

std::string loaded_library_path() {
    Dl_info info{};
    if (!::dladdr(reinterpret_cast<void*>(&arv_get_major_version), &info) || !info.dli_fname) return "<unknown>";
    return info.dli_fname;
}

} // namespace aravis_fake
