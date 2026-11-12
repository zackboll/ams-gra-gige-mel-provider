#pragma once

#include "pleora_backend.hpp"
#include <iostream>

namespace gige_mel::backend::pleora {

// Immutable observation, not an SDK license-state override. The single public
// Connect operation remains authoritative, including in receiver evaluation.
struct LicenseStatus {
    bool valid;
    std::string message;
};

inline Status connect_with_license(const LicenseStatus& license, const Options& options,
                                  const std::function<Status()>& connect) {
    const auto report = [&](const std::string& message) {
        if (options.diagnostic) options.diagnostic(message);
        else std::clog << message << '\n';
    };
    report("eBUS license valid=" + std::string(license.valid ? "true" : "false") +
           ": " + license.message);
    if (!license.valid && !options.allow_watermarked_evaluation)
        return Error{ErrorCode::unsupported, "eBUS SDK license rejected: " + license.message};
    if (options.allow_watermarked_evaluation)
        report("EVALUATION ONLY, NOT PRODUCTION QUALIFIED; SDK development entitlement unresolved");
    if (!license.valid)
        report("UNLICENSED / WATERMARKED receiver evaluation; watermark and SDK limits remain intact");
    report("Attempting normal exclusive Connect (no license override or fallback)");
    auto result = connect();
    report(result ? "eBUS exclusive Connect: OK" : result.error().message);
    return result;
}

} // namespace gige_mel::backend::pleora