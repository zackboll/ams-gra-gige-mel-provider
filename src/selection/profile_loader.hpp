#pragma once
#include "backend_catalog.hpp"

namespace gige_mel::selection {
/// @brief Validated module admission result. Treat as immutable after load.
/// Probe, factory, selected profiles and each wrapped opened state pin the
/// module. Profile destroy and retirement of module-side state/ownership run
/// before releasing their pins. The loader releases its handle by RAII, not by
/// force-unloading while any such owner survives.
struct LoadedProfile {
    BackendCatalog::Profile profile; ///< Probe used to verify identity, not a selected session profile.
    std::uint32_t compatible_backends; ///< Declared adapter mask, not physical-device evidence.
    BackendCatalog::ProfileConstructor create; ///< Construct and check an independent object per selection.
};

/// @brief Admit a trusted matched-C++ module through its explicit absolute path.
/// @param[in] absolute_path Nonempty, NUL-free absolute library path. No search
/// or fallback. Loads with immediate symbol resolution and local visibility.
/// @return Probe and owning factory after verifying the discovery symbol,
/// descriptor API/size/interface, exact build ID, nonempty terminated metadata,
/// known backend bits and callbacks, and a successful non-null matching factory
/// result. Invalid path is invalid_argument; open failure is not_found;
/// incompatible descriptor/discovery/probe failures are unsupported, with
/// allocation and unexpected loader exceptions translated separately.
/// @warning Module initialization can run before validation. No sandbox or
/// authenticity check; establish provenance, toolchain/ABI and build parity
/// independently. Failed admission retains no usable profile registration.
/// @see gige_mel::backend::profiles::module::Descriptor
backend::Result<LoadedProfile> load_profile_module(const std::string& absolute_path);

/// Register the independent factory under the probe ID, before compatible
/// backend registration. Reject missing probe/factory; catalog ID rules apply.
/// Does not bind a backend or enforce the compatibility mask for custom composition.
backend::Status register_loaded_profile(BackendCatalog&, const LoadedProfile&);
} // namespace gige_mel::selection