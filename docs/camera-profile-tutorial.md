# Create a Camera Profile

Add a camera model to an existing GigE MEL Provider backend without adding
camera-specific branches to the MEL facade. Use the [Cognex CIC-5000R module](../camera_profiles/cognex_cic_5000r/README.md)
as the real-camera reference and the [rate-limited module](../examples/profile_module/README.md)
to learn the module and state lifetimes without requiring a camera. Keep the
[camera-profile authoring contract](camera-profile-authoring.md) open for the
detailed interface requirements.

## Contents

1. [Choose the extension boundary](#1-choose-the-extension-boundary)
2. [Implement the profile and open-time admission](#2-implement-the-profile-and-open-time-admission)
3. [Map typed native features](#3-map-typed-native-features)
4. [Implement the per-device state](#4-implement-the-per-device-state)
5. [Build a matched-C++ module](#5-build-a-matched-c-module)
6. [Register and select explicitly](#6-register-and-select-explicitly)
7. [Walk through the Cognex reference](#7-walk-through-the-cognex-reference)
8. [Qualify your profile](#8-qualify-your-profile)

## 1. Choose the extension boundary

| Layer | Owns |
| --- | --- |
| Common MEL provider | MEL channels, commands, metadata, buffer registration, image publication, lifecycle, callbacks, and capability advertisement. |
| Camera backend | Native SDK calls, discovery/open/close, transport, acquisition, buffer leases, and SDK-specific errors. |
| Camera profile | Model/firmware admission, feature mappings, configuration constraints and write order, and admission of frame representations. |
| Optional device capability sources | Navigation acceptance, LOS metadata, stacked-image products, and other device-dependent behavior when actually implemented. |

A profile returns camera facts and policy decisions. It does not implement MEL
channels, deliver buffers, or confer support for optional capabilities. For
example, accepting Mono8 frames does not prove the device produces LOS data or
has a qualified timestamp source. Read the [architecture](architecture.md) and
the [MEL traceability inventory](mel-traceability.md) for those boundaries.

Aravis and Pleora can each pass their own implementation of the same typed
feature service to one profile. The profile makes the same decisions given the
same facts; each backend still has to correctly obtain those facts and deliver
native frames and leases. A new SDK belongs in [backend authoring](backend-authoring.md).
GenTL's current internal adapter is a read-only foundation, not a catalog target
for these modules and not a streaming qualification.

**C++ Interface:** start with the [internal extension reference](index.md#camera-profile-c-interface),
then open [Camera Profile Examples](camera-profile-examples.md) for the reference
implementations. These are source-tree interfaces, not installed public API.

## 2. Implement the profile and open-time admission

Implement [`ICameraProfile`](../src/profiles/camera_profile.hpp). Apart from
destruction it has exactly two operations:

- `std::string_view id() const noexcept`: use an exact, stable selection ID.
  Use a literal or storage valid for the profile's lifetime. Use this same ID
  in the module descriptor and catalog; do not use a discovery/display name.
- `Result<std::shared_ptr<const ICameraProfileState>> open(IDeviceFeatures&) const`:
  called by an adapter after opening the native device. Check identity before
  reading configuration nodes. Discover required features, validate the native
  contract and return a nonnull, independently owned state. Return an error for
  unsupported facts; propagate failed reads with their backend code and text,
  adding feature context where needed. Do not write to make a device pass.

For example, the [Cognex class](../camera_profiles/cognex_cic_5000r/profile.hpp)
returns `"cognex_cic_5000r_v1"`. Its implementation performs this check for each
required identity field (excerpt from `identity`, in the backend namespace and
returning `Status`):

```cpp
auto actual = features.read_string_or_enum(name);
if (!actual)
    return Error{actual.error().code, std::string(name) + ": " + actual.error().message};
if (actual.value() != expected)
    return Error{ErrorCode::unsupported, std::string(name) + " is '" + actual.value() +
        "'; cognex_cic_5000r_v1 requires exactly '" + std::string(expected) + "'"};
```

After the checks, Cognex moves a validated native snapshot and capabilities into
`std::make_shared<CognexCIC5000RState>(...)`, returned as a
`std::shared_ptr<const ICameraProfileState>`. Build one state per open; do not
share evolving device configuration across opens or retain the borrowed feature
service, a native handle, or a pointer back to the parent profile. A stream or
lease may keep the state alive after the profile or device closes.

Constructors and factories must not discover, open, or configure devices.
Registration and ordinary selection bind IDs only. Loading can execute module
code and create a probe profile, but that code must honor the same no-camera-I/O
rule. Neither a matching ID nor a successful selection admits a physical device.

## 3. Map typed native features

Use [`IDeviceFeatures`](../src/device_features.hpp) for typed scalar reads,
implementation and enum-entry queries, numeric values with bounds/increments
and declared units where supplied, and checked integer/float writes. There are
no Aravis, Pleora or GenTL types in the profile. A `read_write` numeric query
checks write access without performing a write. The adapter checks native
node type, availability, access, and write-time constraints, and preserves
SDK error provenance. The profile chooses which nodes, units and combinations
it admits. This interface has no enum setters or arbitrary node-map access;
do not add a generic feature framework for a model-specific rule.

Use this mapping from the [Cognex implementation](../camera_profiles/cognex_cic_5000r/profile.cpp)
and its shared [snapshot reader](../src/profiles/mono_free_running_device.cpp)
as a template for recording what a new model needs:

| Camera behavior | Native feature | Cognex policy |
| --- | --- | --- |
| Vendor and model admission | `DeviceVendorName`, `DeviceModelName` | Exactly `COGNEX`, `CIC-5000R`. |
| Firmware admission | `DeviceFirmwareVersion` | Exactly `106349-08;U;CIC-5000R;V3.7-1;0`. |
| Exposure | `ExposureTimeAbs` | Microseconds; manual exposure (`ExposureAuto=Off` if implemented). |
| Frame rate | `AcquisitionFrameRateAbs` | Hz; explicit set of admitted integer rates. |
| Frame-rate enable state | `AcquisitionFrameRateEnable` | Required and `true`; never enabled implicitly. |
| Trigger state | `TriggerMode` | Required and `Off`; `TriggerSelector=FrameStart` if implemented. |
| Acquisition mode | `AcquisitionMode` | Required and `Continuous`; never changed. |
| ROI | `Width`, `Height`, `OffsetX`, `OffsetY` | Integer pixels; validate against native limits and `SensorWidth`/`SensorHeight`. |
| Pixel representation | `PixelFormat` | Actual `Mono8`, with that entry available; no format switches. |

Do not fall back to an alias because a query failed or the preferred node is
currently inaccessible. For example, the shared exposure lookup tries
`ExposureTime` before `ExposureTimeAbs`, but only advances on confirmed absence:

```cpp
// In a backend alias lookup returning Result<std::string>:
for (const auto name : {"ExposureTime", "ExposureTimeAbs"}) {
    auto exists = features.implemented(name);
    if (!exists) return exists.error(); // query failed: propagate, don't try another name
    if (exists.value()) return std::string(name); // type/access checked on the read
    // Confirmed absent or unimplemented: try the next alias.
}
return Error{ErrorCode::unsupported, "no supported exposure feature"};
```

Similarly, a confirmed-absent optional node can be represented as `std::nullopt`
if your policy allows it. An implemented but unreadable or wrong-typed node is
not absent. Cognex additionally requires the aliases selected by the shared
reader to be exactly the two `Abs` names above.

## 4. Implement the per-device state

Implement all required operations of [`ICameraProfileState`](../src/profiles/camera_profile.hpp).
Separate immutable open-time facts (admitted identity, native mapping and
representation, validated capability envelope) from fresh values (ROI, exposure,
rate, availability, current limits and increments). The adapter serializes
native transactions under its device lock. Use its synchronous, borrowed
connection and before-write guards without reentering the adapter.

### `capabilities()` and optional `native_diagnostics()`

Return the validated open-time capability snapshot without I/O. A scalar minimum
and maximum describe an envelope, not every accepted setting or combination.
Keep a separate policy predicate for discrete values. `native_diagnostics()`
can return owned name/value text such as the admitted firmware and rate set;
do not make SDK calls or present saved facts as live readings.

### `configuration(features, check)`

Return a **complete** `CameraConfiguration`: width, height and both offsets in
pixels, exposure in nanoseconds, and integer Hz. Use fresh native values, not a
previous request or the capability envelope. In the shared monochrome policy,
the connection check brackets the snapshot read; the conversion checks units
and representability, converts native microseconds to nanoseconds, and reports
a nonrepresentable fractional rate rather than silently rounding it to an
integer Hz. See [`read_configuration`](../src/profiles/mono_free_running_device.hpp)
and [`configuration_from`](../src/profiles/mono_free_running.hpp). Cognex can
read back a representable native 16 Hz even though it will not apply or run at
that rate. Report actual native state (or why it cannot be read/represented).

### `apply(features, request, check, before)`

1. Treat the request as a complete snapshot, not a patch. Reject unsupported
   policy (e.g. an inadmissible rate) before any write. Read fresh native facts;
   validate units, bounds, increments, representation, and the coupled ROI and
   timing constraints before executing a plan. Validate deferred constraints
   at the appropriate write if a preceding write changes their native bounds.
2. Order writes according to camera dependencies. In [`plan_writes`](../src/profiles/mono_free_running.cpp),
   each ROI axis avoids temporarily exceeding sensor bounds (e.g. shrink a
   dimension before moving its offset outward when needed). ROI precedes
   timing. Lowering rate lengthens the frame period: write rate first, then
   check exposure against its new native maximum on write. Otherwise write
   exposure before rate. Do not copy that order to a different native feature
   dependency without testing it.
3. Run the before-write guard before each setter. Have the typed adapter
   enforce fresh access, type, availability and numeric constraints at the
   write, including limits changed by earlier writes. On a setter failure,
   keep the original error code/text, identify successful earlier writes and,
   if accessible, read actual state; otherwise report why it is unavailable.
   **No implicit rollback**, even if a later write or final readback fails.
   The shared helper returns a failed before-write guard's error directly;
   do not assume that cancellation/disconnect at that point undid prior writes.
4. After all writes, read and return actual configuration. Native quantization
   can leave a different value from the request. Do not echo or clamp the result
   to your admission policy. Pixel format and acquisition/trigger modes are
   admission facts here, not part of a request to change those nodes.

### `preflight(features)`

Before allocating a pool or starting native acquisition, read again. Verify
native acquisition mode and trigger state and confirm the *effective* ROI,
timing and pixel representation are valid for this run. Cognex checks
`Continuous`, `Off`, enabled rate control, actual `Mono8`, shared snapshot
constraints, and its discrete rate policy. Its `preflight` returns the checked
configuration without changing it. The backend freezes it and the selected
format for frame validation and separately checks `PayloadSize` and its pool
budget (see [`plan_pool`](../src/profiles/frame_policy.hpp)). If the expected
representation changed since open, reject it rather than silently switching.

### `validate_frame(facts, frozen, format)`

Make a decision from [`NativeFrameFacts`](../src/profiles/frame_policy.hpp),
the frozen configuration and selected format. No SDK calls or mutable device
reads in the receive path. Use immutable data or thread-safe instrumentation;
the method must support concurrent receive. The backend owns the buffer and its
lease, not the profile.

- Classify native incomplete status before relying on image facts that may not
  be readable on failure. Return `incomplete` or `reject` with a precise reason;
  neither verdict publishes an image. Use the native frame ID, original SDK
  status and payload facts where applicable; do not invent enum values or
  translate a profile rejection into a fabricated native transport status.
- Check the selected PFNC format, storage/significant bits and byte order; for
  this shared policy require a plain, single 2D image. Verify dimensions and
  offsets against the frozen ROI. Check row bytes, allowed padding and stride.
  Mono8 (`0x01080001`) uses one byte per pixel with no byte order. Unpacked
  Mono12 (`0x01100005`) uses unsigned LSB-aligned 16-bit words with 12 significant
  bits and a zero high nibble, not packed 12-bit rows. The shared policy only
  admits known little-endian Mono12.
  Establish native byte order independently (e.g. qualified device/transport
  evidence and a documented endianness feature); PFNC code, host byte order
  and a fixture claiming `Little` do not establish it for another camera. See
  [Mono12 representation and qualification](mono12-image-profile.md).
- Compute packed row bytes and the addressable image span
  `(height - 1) * stride + packed_row_bytes` using checked multiplication and
  addition. Treat valid image-part size, received payload size, and allocation
  capacity as distinct. Check the part offset and span inside received bytes,
  and received bytes inside capacity, guarding subtractions against underflow.
  Spare allocation cannot make up for missing bytes. The shared validator
  allows nonnegative X padding and no Y padding, includes no trailing row
  padding in the published span, and rejects extra non-image data or hidden
  prefixes. Report accepted stride/span only after the layout is proven.

The shared validator currently rejects invalid plain-image extents safely but
can describe a hidden prefix or excess bytes as “shorter than row stride.” Treat
that wording as a layout rejection, not a measurement of truncation. See
[the diagnostic follow-up](https://github.com/zackboll/ams-gra-gige-mel-provider/issues/52).

Cognex delegates Mono8 validation to the shared algorithms and rejects other
selected formats. The teaching module delegates too, but then rejects odd
native frame IDs to demonstrate that the selected policy runs on receive. That
parity rule is instrumentation, not a practice to copy for a real camera.

## 5. Build a matched-C++ module

Start with the [Cognex entry point](../camera_profiles/cognex_cic_5000r/module.cpp)
and [`profile_module.hpp`](../src/profiles/profile_module.hpp). In the teaching
module, the [rate-limited policy](../examples/profile_module/rate_limited_policy.hpp)
owns a delegated monochrome state and atomically counts calls. It caps the
advertised maximum at 20 Hz, rejects requests and preflight above that cap,
and still returns truthful readback. Read the [teaching descriptor](../examples/profile_module/module.cpp)
with `MODULE_VARIANT=0`; the other variants exist only for negative tests.

1. Export exactly `gige_mel_camera_profile_module_v1` with C linkage and
   default visibility. It is a `noexcept` function returning a borrowed
   pointer to an immutable module-owned descriptor valid while mapped. Fill
   in `api_version`, `sizeof(Descriptor)`, `interface_version`, generated
   `GIGE_MEL_PROFILE_BUILD_ID`, the exact profile ID, vendor and module version.
   Keep the text fields nonempty and NUL-terminated. Set only the explicit
   backend flags you support (`module::aravis`, `module::pleora`, or both).
   Neither flags nor version metadata certify a camera or timing behavior.
2. In the `noexcept` create callback, use `module::guarded_create` around
   allocation. Success is zero with a nonnull output object; failure is nonzero
   with a bounded, NUL-terminated message when capacity permits. The buffer
   belongs to the host: do not retain/free it. Pair the factory with a matching
   `noexcept` `destroy` in the module (Cognex uses `new`/`delete`). The host
   never deletes the raw module profile itself. No camera I/O in these callbacks.
3. Compile only policy code and SDK-free helpers into the module. The
   [standalone Cognex CMake project](../camera_profiles/cognex_cic_5000r/CMakeLists.txt)
   compiles helpers directly; the [teaching CMake project](../examples/profile_module/CMakeLists.txt)
   puts them in a private PIC archive. Both use C++20, hidden visibility,
   an ELF version script to localize everything except discovery, and
   `-Wl,-z,defs` to catch unresolved dependencies. GCC builds also disable
   GNU unique symbols so they do not cause the loader to retain the module.

Build the host and module from the same revision and with the same compiler
and version, libstdc++, C++ mode/ABI settings, `CMAKE_BUILD_TYPE`, general
and configuration-specific `CMAKE_CXX_FLAGS`, and `GIGE_MEL_SANITIZERS`.
[`ProfileBuildIdentity.cmake`](../cmake/ProfileBuildIdentity.cmake) generates
`profile_build_identity.hpp` for both builds. The identity includes the
hashed interface header closure and flags, compiler, standard-library/ABI
and debug-STL settings, language mode, sanitizer selection and Linux x86-64
target. Never edit the generated identity to get a mismatch past the loader.
Only discovery is C linkage: virtual calls, objects and `shared_ptr` ownership
cross a matched-C++ boundary, not a stable cross-toolchain C ABI.

From the matching repository root, for a host built with `/usr/bin/c++` in
Debug with default flags and no sanitizers (adjust both builds together):

```sh
root="$PWD"
cmake -S "$root/examples/profile_module" -B "$root/build/rate-limited-standalone" -G Ninja \
  -DGIGE_MEL_SOURCE_ROOT="$root" -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_BUILD_TYPE=Debug
cmake --build "$root/build/rate-limited-standalone" --parallel 2

cmake -S "$root/camera_profiles/cognex_cic_5000r" -B "$root/build/cognex-standalone" -G Ninja \
  -DGIGE_MEL_SOURCE_ROOT="$root" -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_BUILD_TYPE=Debug
cmake --build "$root/build/cognex-standalone" --parallel 2

for module in "$root/build/rate-limited-standalone/librate_limited_profile.so" \
              "$root/build/cognex-standalone/libcognex_cic_5000r.so"; do
  nm -D --defined-only "$module"
  readelf -h -d "$module"
done
```

Expect just the discovery symbol among defined dynamic exports, an x86-64
ELF shared object (`DYN`), no provider or camera SDK in `DT_NEEDED`, and no
`NODELETE` flag. Standard C++ and system-runtime dependencies are expected.
These checks do not replace loading and lifetime tests. Neither build needs
SDK headers/libraries or camera access.

[`load_profile_module`](../src/selection/profile_loader.hpp) takes an explicit
absolute `.so` path. It opens with `RTLD_NOW | RTLD_LOCAL`, checks symbol and
descriptor, and calls the factory once to verify the returned ID against the
descriptor. The resulting `LoadedProfile` has probe metadata and a factory for
independent selection instances. Treat it as immutable. Loading runs trusted
executable code (even ELF initialization before descriptor validation); it
provides no sandbox or authenticity guarantee.

The loader pins the module through factories, profiles and **each state**.
During teardown, stop and retire acquisition using the normal host lifecycle,
release outstanding leases and drop devices, profiles and catalog/module
references when no longer needed. A retained state must keep working after its
parent profile is gone: the wrapper drops the module-owned state's `shared_ptr`
(including its deleter/control block) before dropping the state's pin. Each
profile's destroy callback runs while its pin is live. Only the final pin can
release the loader handle; never force-unload. The teaching finalizer tests
this ordering; it does not manage lifetime.

## 6. Register and select explicitly

Use the [internal selector example](../examples/select_session.cpp) as the host
entry point (built with `GIGE_MEL_BUILD_EXAMPLES=ON`). The convenience path below
loads and registers the module and binds a session; `take` is the example's
helper that returns a success value or throws with the error message:

```cpp
namespace s = gige_mel::selection;
auto module = take(s::load_profile_module(absolute_module_path));
auto catalog = take(s::make_builtin_catalog({module}));
auto selected = take(s::select_session(catalog, {
    "aravis", exact_aravis_device_id, "cognex_cic_5000r_v1", {}}));
auto manager = gige_mel::make_api_manager(selected.settings, selected.backend);
if (!manager) throw std::runtime_error("manager construction failed");
```

For custom composition, own a [`BackendCatalog`](../src/selection/backend_catalog.hpp)
and register profiles before backends. For a statically linked Cognex class with
the Aravis target available, `require` below checks a `Status` and throws on
error; `b` aliases `gige_mel::backend`:

```cpp
s::BackendCatalog catalog;
require(catalog.register_profile("cognex_cic_5000r_v1",
    []() -> b::Result<s::BackendCatalog::Profile> {
        return s::BackendCatalog::Profile(
            std::make_shared<b::profiles::cognex::CognexCIC5000RProfile>());
    }));
require(catalog.register_backend("aravis",
    std::vector<std::string>{std::string(s::mono_free_running_v1), "cognex_cic_5000r_v1"},
    [](s::BackendCatalog::Profile profile) { return b::aravis::make_backend(std::move(profile)); }));
```

For a loaded module in a custom catalog, use
`require(s::register_loaded_profile(catalog, module))` **instead of** the
static registration; then include its ID in the backend's compatibility list
only if the corresponding `module.compatible_backends` flag is set and the
adapter supports it. `register_loaded_profile` registers the factory, not a
backend. [`make_builtin_catalog`](../src/selection/builtin_backends.hpp) does
both steps for its compiled adapters and filters by descriptor flags. Do not
register the same ID twice. Synthetic is fixed to `mono_free_running_v1`; it
cannot run these modules. Unknown IDs or unlisted pairs fail without fallback.

With the optional targets compiled into the host (`GIGE_MEL_BUILD_ARAVIS=ON`
and/or `GIGE_MEL_BUILD_PLEORA=ON`), select explicitly. Replace `build/native`
with the host build path. For Pleora set up the external SDK runtime as in
[Pleora integration](pleora-integration.md) first. The arguments below are
placeholders for the exact Aravis ID or exact Pleora lowercase MAC without a
`mac:` prefix. An ID in one backend's namespace is not an ID in the other's.

```sh
module="$PWD/build/cognex-standalone/libcognex_cic_5000r.so"
./build/native/gige_mel_select --profile-module "$module" --list
./build/native/gige_mel_select --profile-module "$module" aravis EXACT_ARAVIS_DEVICE_ID cognex_cic_5000r_v1
./build/native/gige_mel_select --profile-module "$module" pleora EXACT_LOWERCASE_MAC cognex_cic_5000r_v1
```

To exercise the teaching module instead, use `librate_limited_profile.so`
from the first build and `test_rate_limited_v1` as the profile ID. `--list`
lists registered **backends**, not attached cameras. Ordinary
[`select_session`](../src/selection/session_selection.hpp) binds the exact,
nonempty device ID into settings; `settings.instance` is not a selector.

| Operation | Camera effects |
| --- | --- |
| Load module, register, select, construct manager | No discovery, open, feature access, writes or acquisition in a conforming composition. |
| Backend discovery | Explicit native enumeration; not performed by `--list` or selection. |
| `--inspect` | Explicitly opens (may take native control), evaluates profile admission, reads configuration and closes. No apply or acquisition. |
| Backend open or MEL control `init()` | Opens and checks device/profile admission; not a request to apply settings or start acquisition. |
| Explicit configuration apply; explicit acquisition start | Apply can write; start preflights and then uses native acquisition. Follow the existing [lifecycle](architecture.md). |

The exported `getAPI_Manager("synthetic")` factory does not accept arbitrary
real-camera profiles. Use explicit source-tree composition and the C++ manager
embedding interface. See [runtime selection](runtime-selection.md) for the
full ownership and ID contract.

## 7. Walk through the Cognex reference

Follow [Camera Profile Examples](camera-profile-examples.md) for file and source
browser links. Read the Cognex files in this order:

1. **`profile.hpp` → `identity` in `profile.cpp`.** The class supplies one ID
   and admits only the exact vendor/model/firmware strings in the table above.
   No serial-number restriction, firmware wildcard or generic fallback.
2. **`open` → `read_snapshot` → `native_contract`.** Identity comes first.
   Discover the typed feature mapping and capabilities without writes. Require
   enabled rate control, continuous free-running Mono8 and manual exposure.
   Own the native envelope and policy capabilities as separate values.
3. **`admit_rate` → `capabilities`.** The policy set is **1, 2, 4, 5, 8 and
   10 Hz**, subject to native constraints. Capabilities bound that set by the
   open-time native range. An advertised 1–10 Hz envelope does not admit 3, 6,
   7 or 9 Hz, or guarantee all ROI/exposure combinations. The limit is policy,
   not a physical maximum.
4. **`configuration` and `apply`.** Delegate conversion and the ordered
   write transaction to the shared monochrome code. Apply the rate policy
   before writes but do not clamp actual readback. Retain the *native* envelope
   in these helpers so an existing representable 16 Hz state can be read and
   explicitly corrected to an admitted rate without pretending it was 10 Hz.
5. **`preflight`.** Re-read the native contract and effective configuration,
   then apply the discrete rate policy. A native 16 Hz state cannot start a run.
6. **`validate_frame`.** Check the selected Mono8 representation and delegate
   layout/span validation. Keep native IDs; do not copy the example's parity
   rule. Cognex rejects Mono12 even with a fixture claiming little-endian data.
7. **`module.cpp`.** `guarded_create` allocates the profile, `destroy` deletes
   it, and the descriptor declares both Aravis and Pleora, not GenTL.
8. **`CMakeLists.txt`.** Compile the policy and helpers into a standalone
   `MODULE` target with matching identity and the single exported symbol.
9. **Host composition.** Pass the module path, exact backend and device ID and
   `cognex_cic_5000r_v1` as in step 6. Admission happens at open, not selection.
10. **Evidence.** The [module validation record](tasks/GIGE-MEL-036-validation.md)
    covers SDK-free typed fixtures, Aravis Fake and native Pleora GenApi/PvBuffer
    fixtures. The [earlier physical observations](cognex-cic-5000r.md) are
    separate: the strongest prior streaming evidence is full-frame 2592×1944
    Mono8 at 2, 5 and 10 Hz over the observed 1 Gb/s link, with loss observed
    under heavy load. Exactly representable rates (including 16 Hz) are not
    proof of reliable delivery. The module itself is fixture-qualified only;
    no new firmware, ROI, native Mono12 byte order, production timestamps or
    physical streaming/timing qualification is inferred. Pleora development
    entitlement and production licensing are separate gates.

## 8. Qualify your profile

Record the tested profile revision, backend/SDK version, device or fixture
identity/firmware, inputs and observed results. Use the checklist for each
declared backend. Keep failures and untested combinations visible.

**Identity and feature admission**

- [ ] Accept the exact supported vendor/model/firmware; reject unknown or
  mismatched identities before configuration discovery and with zero writes.
- [ ] Test confirmed absence, failed queries, unavailable or unreadable nodes,
  wrong types, and aliases (including failure of the preferred alias).
- [ ] Verify native bounds, increments and declared units, including invalid
  numeric facts and nonrepresentable values.

**Configuration behavior**

- [ ] Exercise a valid full request and actual readback, including quantization.
- [ ] Reject unsupported rates and pixel formats without implicit mode changes.
- [ ] Test coupled ROI/exposure/rate constraints, fresh limits before writes,
  dependency changes during apply, correct ordering, and failure after earlier
  successful writes. Verify the reported error and actual partial state or
  precise readback failure; verify no rollback.
- [ ] Test fresh preflight failure for unacceptable mode, trigger, representation
  or effective settings before pool allocation/start.

**Frames and lifecycle**

- [ ] Test accepted, incomplete and rejected frames; geometry, offsets, stride,
  pixel bits/byte order, span overflow and received-vs-capacity bounds. Verify
  native diagnostic provenance and no image publication on rejection.
- [ ] Verify native lease bytes after stop/close, cancellation of a blocked
  receive, disconnect, and thread-safe validation with frozen settings.

**Module and selection**

- [ ] Reject wrong API/interface version, descriptor size/metadata, build
  identity, backend flags, and null/throwing factories. Check that incompatible
  descriptors are rejected before factory execution and a returned ID must match.
- [ ] Reject duplicate IDs and unsupported backend/profile pairs without
  fallback. Assert no discovery, open, configuration or acquisition during
  registration/loading and ordinary selection.
- [ ] Verify independent profile/state instances and release order: the state
  can outlive the profile; state and profile destruction finish before unload.

| Evidence tier | Existing starting points and what they prove |
| --- | --- |
| SDK-free contracts | [`camera_profile_contract`](../tests/camera_profile_tests.cpp) tests shared configuration/frame policy. [`device_feature_contract` and `dynamic_profile_module_contract`](../tests/device_feature_tests.cpp) cover typed queries, apply and lifecycle/error cases including invalid DSOs and actual unload. [`cognex_profile_contract`](../tests/cognex_profile_tests.inc) exercises exact admission and Cognex policy. [`selection_contract`](../tests/selection_tests.cpp) and [`builtin_selection_contract`](../tests/builtin_selection_tests.cpp) cover registration and binding without camera I/O. See [module validation](tasks/GIGE-MEL-035-validation.md) for negative tests. |
| Native SDK fixtures | [`aravis_adapter`](../tests/aravis_adapter_tests.cpp) uses Aravis Fake, including the loaded Cognex policy. [`pleora_sdk_contract`](../tests/pleora_contract_tests.cpp) exercises native GenApi/PvBuffer; this is not a connected Cognex device. See the [Cognex validation record](tasks/GIGE-MEL-036-validation.md). |
| Simulated transport | [`aravis_gige_loopback_contract`](../tests/aravis_gige_loopback_contract.cpp) exercises loopback GigE transport. Distinguish packet/receive and controlled failure behavior from in-process fixtures, and neither from physical camera results. |
| Physical camera | Link a separately authorized record with exact firmware, topology, formats, settings, loss and timing limits. The [Cognex physical record](cognex-cic-5000r.md) is not a qualification of this module. Read identity and snapshot initial state before any approved changes; restore and independently verify the original settings. Never make hardware a default CI requirement. |

Start with the [SDK-free development suite](../README.md#sdk-free-quick-start),
build your module separately, and run the relevant existing native fixtures
only where the SDKs are available. Do not use `--inspect`, enumerate devices or
enable hardware test lanes for a no-camera documentation or offline validation run.