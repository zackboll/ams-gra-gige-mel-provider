#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

// Test-process-only actual module finalization observation, not loader ownership.
class ModuleUnloadProbe {
public:
    explicit ModuleUnloadProbe(bool enabled) : enabled_(enabled) {
        if (!enabled_) return;
        char name[] = "/tmp/gige-mel-module-XXXXXX";
        const int fd = mkstemp(name);
        if (fd < 0) throw std::runtime_error("mkstemp unload probe failed");
        close(fd);
        path_ = name;
        if (setenv("GIGE_MEL_TEST_UNLOAD_LOG", path_.c_str(), 1))
            throw std::runtime_error("setenv unload probe failed");
    }
    ~ModuleUnloadProbe() {
        if (enabled_) { unsetenv("GIGE_MEL_TEST_UNLOAD_LOG"); std::filesystem::remove(path_); }
    }
    void expect(unsigned count) const {
        if (!enabled_) return;
        std::ifstream file(path_);
        unsigned actual = 0;
        for (std::string line; std::getline(file, line);) ++actual;
        if (actual != count) throw std::runtime_error("actual module unload count " + std::to_string(actual) +
            " expected " + std::to_string(count));
    }
private:
    bool enabled_;
    std::string path_;
};