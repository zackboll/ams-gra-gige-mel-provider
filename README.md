# AMS-GRA GigE MEL Provider

A native C++20 **provider** of the AMS-GRA infrared MFA Encapsulation Layer
(MEL), with a shared MEL runtime and interchangeable camera backends. MEL is
not Matrox MIL. The separate
[`ams-gra-mel-bridge`](https://github.com/zackboll/ams-gra-mel-bridge) is a
consumer bridge, not a dependency.

**Status: experimental, partial MEL image-stream implementation.** The SDK-free
synthetic camera delivers images into consumer-registered host memory. Optional
Aravis and Pleora adapters implement real source acquisition, with limited
physical Cognex capture evidence, but their production timestamps remain invalid:
the MEL provider rejects those frames. This is **not full MEL 4.0 conformance,
production timestamp qualification or a production-qualified camera driver**.

Start with the **[documentation index](docs/index.md)**, the
[capability and evidence status](docs/status.md), and the
[implementation profile](docs/implementation-profile.md).

## SDK-free quick start

Prerequisites: Linux, a C++20 compiler (GCC or Clang), CMake **3.24+**, Ninja,
Python **3.9+**, and POSIX threads. Pinned MEL headers and the private JSON
parser are checked in; configure/build does not download dependencies. No camera,
proprietary SDK or external network access is needed. The full test suite includes
local loopback transport tests; it does not contact physical cameras.

Run from the repository root:

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev

# Dynamically load the upstream factories and capture a synthetic image.
./build/dev/mel_capture ./build/dev/libgige_mel_provider.so ./build/dev/capture.pgm
```

The native [capture example](examples/capture.cpp) demonstrates factory loading,
channel setup, registered memory and release. The standard factory instance is
exactly `synthetic`; an arbitrary instance string is not a backend selector.

For an installed CMake consumer, follow
[Integrating a MEL Consumer](docs/consumer-integration.md) or build the
[standalone example](examples/installed_consumer/) directly:

```sh
cmake --install build/dev --prefix "$PWD/build/install"
cmake -S examples/installed_consumer -B build/installed-example -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/installed-example
./build/installed-example/capture_one
```

### Build controls

The [CMake definitions](CMakeLists.txt) and [presets](CMakePresets.json) are
authoritative. `dev`, `release`, `asan` (Address/UndefinedBehaviorSanitizer) and
`tsan` presets each have matching build/test presets.

| CMake cache control | Default | Purpose / prerequisites |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | CTest contracts and fixture builds |
| `GIGE_MEL_BUILD_EXAMPLES` | `ON` | Native capture and internal selection examples |
| `GIGE_MEL_BUILD_DOCS` | `OFF` | Opt-in Doxygen reference; no effect on normal builds, CTest, or the installed package |
| `GIGE_MEL_WARNINGS_AS_ERRORS` | `ON` | Treat project compiler warnings as errors |
| `GIGE_MEL_SANITIZERS` | empty | GCC/Clang sanitizer list; set by sanitizer presets |
| `GIGE_MEL_BUILD_ARAVIS` | `OFF` | Internal adapter; requires exactly Aravis 0.8.36. [Setup and tests](docs/aravis-integration.md) |
| `GIGE_MEL_BUILD_PLEORA` | `OFF` | Internal adapter; requires external eBUS SDK and matching GenApi headers. [Setup, licensing and runtime](docs/pleora-integration.md) |
| `GIGE_MEL_BUILD_GENTL_PROBE` | `OFF` | Uninstalled diagnostic probe/fixtures; requires explicitly provisioned EMVA GenTL.h. [Probe design/setup](docs/tasks/GIGE-MEL-GENTL-001-design.md) |
| `GIGE_MEL_BUILD_GENTL_BACKEND` | `OFF` | Internal read-only foundation; requires the probe and Aravis 0.8.36 GenApi. [Backend design/setup](docs/tasks/GIGE-MEL-GENTL-002-design.md) |
| `GIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT` | `OFF` | Uninstalled isolated traffic audit; requires Pleora and explicit launch. [Scope and results](docs/pleora-stream-only-audit.md) |

Enabling an SDK build does not change exported MEL factory selection. SDK paths,
licensing, hardware-test opt-ins and network side effects are documented in the
integration guides, not enabled by this quick start.

## Architecture and interface boundaries

```text
Native MEL consumer
  -> pinned upstream C++ factories and channel interfaces
  -> shared runtime: lifecycle, configuration, validation, callbacks, buffers
  -> SDK/MEL-independent C++ backend values and source leases
  -> synthetic backend OR explicitly composed SDK adapter
                              -> camera-profile policy + native SDK/transport
```

The portable image path copies validated source pixels into registered host
destinations. A source lease and a delivered MEL buffer are separate ownership
contracts. Retained destination bytes cannot be overwritten until explicit
`Buffer::release()`. Stop must wake receive; consumer callbacks run outside the
runtime operation lock. Keep the loaded provider library alive until its objects,
callbacks, futures and outstanding buffers have retired. See
[architecture and lifecycle](docs/architecture.md).

| Interface surface | Availability and compatibility |
| --- | --- |
| Upstream MEL headers and `getAPI_Manager` / `getControl` / `getBuffer` | Installed consumer-facing interface. Exact pinned C++ signatures; C linkage is for symbol discovery, **not a C-callable provider API**. Requires a compatible C++ ABI. |
| `include/gige_mel/` | Installed project headers: `provider.hpp` embedding/diagnostics, `backend.hpp` source contract, and navigation/LOS value headers. These are project-owned C++ seams, not a stable runtime plugin ABI; `backend.hpp` stays independent of MEL and vendor SDK types. |
| `src/profiles/`, `src/device_features.hpp`, `src/selection/` | Internal, uninstalled camera-profile, typed feature, catalog, session-selection and module-loader interfaces. SDK adapters and profile targets are also uninstalled. |
| Explicit camera-profile modules | Trusted, opt-in Linux x86-64 matched-C++ implementations. A versioned C-linkage discovery symbol does **not** make virtual calls, STL values or destructors a stable cross-toolchain ABI. Compiler/stdlib/ABI flags, interface revision and sanitizer/build settings must match. No dynamic backend plugin ABI, automatic search or fallback. |

The installed CMake package exports `gige_mel::provider`, `gige_mel::backend`
and `gige_mel::mel_headers`. Internal composition uses
`gige_mel::make_api_manager(settings, backend)`; `getAPI_Manager("synthetic")`
does not select optional adapters. See [internal runtime selection](docs/runtime-selection.md)
and [matched-C++ profile authoring](docs/camera-profile-authoring.md).

## Backend and profile capabilities

| Backend / profile | Implemented | Evidence / limit |
| --- | --- | --- |
| Synthetic / default | SDK-free image delivery, controls, Mono8 and explicit Mono12 | Contract/portability tests; virtual clock and optics only |
| Aravis 0.8.36 / default monochrome | Optional discovery, control/readback and bounded source acquisition | Fake/loopback tests and limited physical Cognex Mono8 observations; **no production MEL image delivery** (invalid timestamps) |
| Pleora eBUS Pv 6.5.3.7155 / default monochrome | Optional control/readback and bounded host-copy source acquisition | Native fixtures and limited watermarked Cognex evaluation; entitlement/production licensing unresolved; **no production MEL image delivery** |
| Cognex `cognex_cic_5000r_v1` module / Aravis or Pleora | Exact model/firmware admission; Mono8 at conservative 1/2/4/5/8/10 Hz | Module fixture-qualified only; earlier physical evidence informs policy, not new module/hardware/time qualification |
| GenTL / injected existing profiles | Optional read-only enumerate/open/readback foundation | Controlled producer fixture only; no apply, streaming, catalog selection or qualified real producer/camera |

See [current status](docs/status.md) for merged capabilities and
[Qualification Evidence](docs/qualification-evidence.md) for the original
experiments and their limits. The provider also has bounded C2/Health software
behavior, navigation receipt/optional forwarding, and source-conditional LOS and stacked
image foundations. LOS/stack positive sources are recording fixtures only;
default synthetic and production adapters do not advertise them. Scheduling,
tracking, calibration, protection and several required MEL surfaces remain
unsupported. The [MEL traceability inventory](docs/mel-traceability.md)
tracks required, conditional and optional work; channel stubs are not conformance.

## Integration and development starting points

- **MEL consumer:** [integration guide](docs/consumer-integration.md),
  [standalone example](examples/installed_consumer/),
  [implementation profile](docs/implementation-profile.md).
- **SDK integration:** [Aravis](docs/aravis-integration.md),
  [Pleora](docs/pleora-integration.md), [runtime composition](docs/runtime-selection.md).
- **New camera profile:** [Create a Camera Profile](docs/camera-profile-tutorial.md),
  [authoring contract](docs/camera-profile-authoring.md),
  [C++ interface](docs/index.md#camera-profile-c-interface),
  [reference implementations](docs/camera-profile-examples.md).
- **New backend:** [backend authoring](docs/backend-authoring.md),
  [portability contracts](docs/backend-portability.md).
- **MEL interface obligations:** [requirements traceability](docs/mel-traceability.md),
  exact pinned [IR MEL](third_party/ir-mel/include/) and
  [Common MEL](third_party/common-mel/include/) declarations.
- **Design and evidence:** [documentation index](docs/index.md#evidence-and-design-history),
  [historical validation record](docs/validation.md), [repository instructions](AGENTS.md).

## Build the documentation

The opt-in `docs` target uses **Doxygen 1.14.0** and Graphviz (`dot`). No
optional camera SDK, producer, or hardware is needed. From the repository root
on Linux x86-64, with Graphviz installed:

```sh
sh scripts/install_doxygen.sh  # official release, checked against a pinned SHA-256
cmake -S . -B build/documentation -G Ninja -DBUILD_TESTING=OFF \
  -DGIGE_MEL_BUILD_DOCS=ON \
  -DDOXYGEN_EXECUTABLE="$PWD/build/doc-tools/doxygen-1.14.0/bin/doxygen"
cmake --build build/documentation --target docs
python3 -m http.server 8000 --bind 127.0.0.1 --directory build/documentation/docs/html
```

Open [the local site](http://127.0.0.1:8000/) while the server is running. The
landing page separates the installed `include/gige_mel/` contract, uninstalled
camera extension interfaces, and internal runtime source reference. The installed
backend headers are a C++ source contract, not a stable cross-toolchain plugin ABI.

The [published documentation](https://zackboll.github.io/ams-gra-gige-mel-provider/)
shows its exact source commit in the footer. Pull requests build and validate
without publishing; only validated `main` pushes can deploy. For profile/backend documentation and API
comment guidance, see [Contributing: documentation](CONTRIBUTING.md#documentation).

## Dependencies and licensing

Pinned upstream revisions, measured include closure and original notices are in
[upstream provenance](docs/upstream-provenance.md) and the
[dependency manifest](third_party/manifest.json). Normal builds verify vendored
MEL and private JSON bytes; SDKs are external and optional. To verify explicitly:

```sh
python3 scripts/vendor_upstream.py --verify
python3 scripts/verify_json.py
```

Original project sources are [Apache-2.0](LICENSE). Third-party licenses and
intent notices remain with their sources; see [NOTICE](NOTICE). No proprietary
SDK files, generated documentation or camera evidence should be committed.