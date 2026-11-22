#pragma once

// Shared native ownership and error helpers; never installed.
#include <gige_mel/backend.hpp>

#include <arv.h>

#include <utility>

namespace gige_mel::backend::aravis::detail {

inline Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

struct ObjectUnref {
    void operator()(gpointer object) const noexcept { if (object) g_object_unref(object); }
};
template <class T> using GObjectPtr = std::unique_ptr<T, ObjectUnref>;

struct ErrorFree {
    void operator()(GError* error) const noexcept { if (error) g_error_free(error); }
};
using GErrorPtr = std::unique_ptr<GError, ErrorFree>;

// Collects one GError; owns it on every path.
class ErrorSlot {
public:
    GError** out() noexcept { return &raw_; }
    ~ErrorSlot() { if (raw_) g_error_free(raw_); }
    ErrorSlot() = default;
    ErrorSlot(const ErrorSlot&) = delete;
    ErrorSlot& operator=(const ErrorSlot&) = delete;
    [[nodiscard]] bool failed() const noexcept { return raw_ != nullptr; }
    GErrorPtr take() noexcept { return GErrorPtr(std::exchange(raw_, nullptr)); }

private:
    GError* raw_{nullptr};
};

inline std::string copy(const char* text) { return text ? std::string(text) : std::string(); }

Error translate(std::string_view operation, std::string_view feature, const GError& error);

} // namespace gige_mel::backend::aravis::detail
