# Internal runtime backend and profile selection (GIGE-MEL-033/034)

## Scope and API

This is build-local C++20 composition, not a new MEL consumer interface, public
provider ABI or stable cross-toolchain binary plugin contract. The opt-in
matched-C++ module loader is described below. The built-in
canonical profile is `mono_free_running_v1`. It selects the existing
monochrome/free-running policy; it does **not** rename the historical diagnostic
identifier `aravis-mono8-free-running-v1`, select a new pixel format, qualify
camera clocks or advertise new MEL capabilities.

The explicitly owned `selection::BackendCatalog` in
`src/selection/backend_catalog.hpp` owns separate profile factories and backend
constructors. Each catalog initially registers the default monochrome profile.
`register_profile(id, factory)` adds an exact ID; a profile factory creates a
fresh `shared_ptr<const ICameraProfile>` for each selection. A backend registration
supplies a list of compatible profile IDs and a constructor accepting that
selected profile. Duplicate IDs, empty/NUL IDs, absent callables, unknown profiles
and duplicate compatibility entries are rejected. Selection validates exact
compatibility before calling either factory, checks profile/backend result IDs
and nulls, preserves returned errors and translates constructor exceptions. No
fallback or camera I/O occurs. Constructors must inject rather than discard the
selected implementation; arbitrary application callbacks cannot be policed by
the catalog. The legacy no-argument constructor overload admits only the default
fixed-policy binding (used by synthetic and older embeddings).

Register before concurrent reads; mutation is not synchronized. Custom factories
must construct independent session objects without camera I/O. There is no
process-global registration. See [profile authoring](camera-profile-authoring.md)
for the implementation and ownership contract.

`src/selection/builtin_backends.cpp` is the separate optional-SDK composition
boundary. Its `make_builtin_catalog()` registers `synthetic` always, `aravis`
only with `GIGE_MEL_BUILD_ARAVIS=ON`, and `pleora` only with
`GIGE_MEL_BUILD_PLEORA=ON`. It uses the existing factories with production
defaults (including no Pleora watermarked evaluation). A custom composition may
capture adapter-specific construction options in its constructor, without
putting SDK options or native objects in the catalog.

`SessionSelection` contains backend ID, exact backend-specific device ID,
profile ID and existing `ProviderSettings`. `select_session` validates the
nonempty/NUL-free device identity and supported combination, creates an independent
backend, and binds its `open` operation to that exact device ID. Device IDs are
not trimmed, case-folded, translated from IP/display names or partially matched.
The adapter remains authoritative about existence, compatibility and native
access when explicitly opened. Selection cannot validate those facts offline.

The explicit device ID is authoritative and replaces `settings.device_id`;
all other settings are preserved. In particular `settings.instance` remains a
MEL session label, never a backend selector. Composition uses the existing seam:

```cpp
auto catalog = gige_mel::selection::make_builtin_catalog();
if (!catalog) throw std::runtime_error(catalog.error().message);
auto selected = gige_mel::selection::select_session(catalog.value(), {
    "synthetic", "synthetic-0", "mono_free_running_v1", {}});
if (!selected) throw std::runtime_error(selected.error().message);
auto& session = selected.value();
auto manager = gige_mel::make_api_manager(session.settings, session.backend);
// No discovery, open, inspection, writes or acquisition have occurred.
// Existing getControl(...)->init() opens; normal MEL lifecycle follows.
```

The catalog and binding hold no native devices or streams. The bound backend
delegates explicit discovery/open and returns the original device unchanged.
Device locking, close, exclusive native control, same-adapter ownership
reservations, native errors, source leases, cancellation, timestamps and MEL
publication remain in their existing implementations. No SDK headers enter the
catalog or installed common provider. The internal selection libraries and
example are not installed/exported. `getAPI_Manager("synthetic")` and rejection
of all other factory names remain unchanged.

## Profile and ownership boundaries

Both real adapters consume the selected `ICameraProfile`, which opens an owned
`ICameraProfileState` using a borrowed `IDeviceFeatures`. Open validation,
capabilities, read/apply, preflight and frame admission dispatch through that
state. Each concrete state privately owns its own snapshot type. The default
implementation delegates to the existing monochrome algorithms, retaining aliases,
units, ordering, readback, formats and historical diagnostic ID. Backend/device
cores keep the profile alive; streams keep the state/core alive. Native leases
retain their original storage owners and may outlive close and profile teardown.
States must own their dependencies independently of their parent profile.

Synthetic remains a fixed-policy regression backend with only its existing
compatibility binding; it does not emulate native features or accept alternate
profiles. Aravis and Pleora can each advertise multiple registered profiles.
Static compatibility lists do not certify a physical device: feature discovery
and device-specific admission occur only on explicit open.

Switching means stop acquisition, release/retire the session using the existing
lifecycle, close the current device, and create a new selection. There is no
automatic reconnect or live backend switch. The selector neither transfers
native ownership nor merges unrelated SDK device namespaces. Existing adapter
reservations prevent two instances of the same adapter claiming the same device;
native camera control/exclusive access remains authoritative across SDKs and
processes. Applications must not attempt simultaneous cross-SDK control of one
camera. Offline selection does not establish physical identity equivalence or
qualify cross-SDK contention on physical hardware.

## Uninstalled example

`gige_mel_select` is built with `GIGE_MEL_BUILD_EXAMPLES=ON` (default):

```sh
./build/033-free-dev/gige_mel_select --list
./build/033-free-dev/gige_mel_select synthetic synthetic-0 mono_free_running_v1
./build/033-free-dev/gige_mel_select synthetic synthetic-0 mono_free_running_v1 --inspect
```

Every invocation lists registered IDs without hardware discovery. The second
command only selects/binds and creates a manager. Only `--inspect` explicitly
opens, reads configuration and closes; it never calls `apply`, creates a stream
or starts acquisition. A real-device open involves native control/connection
side effects; it is not passive observation. Readback and close errors are both
reported. Pleora-enabled executables need the existing external SDK runtime
environment described in [Pleora integration](pleora-integration.md).

## Tests and future work

`selection_contract` covers exact IDs, unavailable backends, duplicate/invalid
registration, null/throwing/error constructors, ID mismatches, unsupported
profiles, missing/invalid device IDs, repeated destruction, independent sessions,
no pre-open I/O, MEL-init authority and no fallback. `builtin_selection_contract`
checks every enabled/disabled combination and repeated factory creation without
discovery/open. Three example CTests use only synthetic devices.

Optional native tests select the real factories and run the same representative
requests from `tests/support/selected_profile_contract.hpp` against the real
Aravis Fake device and Pleora's real GenApi feature fixture. Both accept a
300x240, 20 Hz, 1250 us request, reject zero width before writes, return actual
readback and write rate before exposure when lengthening the period. Existing
native tests retain fixture-specific grids, quantization, ordering, partial-write
errors and terminal guards; no identical frame IDs, transport policies or native
error strings are asserted across SDKs. Pleora offline tests do not pretend to
open a physical device. The existing eight-group native TSan audit and its
single-function exception are unchanged.

GIGE-MEL-034's test-only `test_rate_limited_v1` composes the same representation
algorithms with independent state, a <=20 Hz admission rule and odd-frame-ID
rejection. It is registered through the same mechanism, executes in Aravis's
actual Fake open/apply/receive path and Pleora's native feature/copy-stream
fixtures, and is never included in production builtins. The unchanged provider
is additionally exercised through MEL C2 with alternate admission. No connected
Pleora open or physical-camera compatibility is claimed by offline fixtures.

No physical camera tests run
implicitly; no new SDK/camera combination or production timestamp is qualified.

## Explicit module loading

`selection::load_profile_module(absolute_path)` in `src/selection/profile_loader.*`
uses dlopen with `RTLD_NOW | RTLD_LOCAL`, dlsym of exactly
`gige_mel_camera_profile_module_v1`, dlerror diagnostics and RAII dlclose.
Relative, empty and embedded-NUL paths fail. Missing files/symbols, malformed or
incompatible descriptors, factories and returned IDs fail without another profile
being selected. Modules are trusted executable code: constructors may run before
validation. There is no download, directory search, auto-discovery or sandbox.

The returned `LoadedProfile` owns validated profile metadata and an independent
profile factory. `register_loaded_profile(catalog, loaded)` registers its exact
ID; duplicate IDs fail. Register before backend compatibility lists. The overload
`make_builtin_catalog(vector<LoadedProfile>)` first registers modules, then composes
only explicitly declared Aravis/Pleora compatibility; synthetic stays default-only.
A custom composition must likewise explicitly declare and inject the selected
profile. Compatibility promises source-contract support, not camera qualification.

```cpp
auto loaded = gige_mel::selection::load_profile_module("/absolute/profile.so");
if (!loaded) throw std::runtime_error(loaded.error().message);
auto catalog = gige_mel::selection::make_builtin_catalog({loaded.value()});
if (!catalog) throw std::runtime_error(catalog.error().message);
auto session = gige_mel::selection::select_session(catalog.value(), {
    "aravis", "EXACT_DEVICE_ID", std::string(loaded.value().profile->id()), {}});
```

**Optional capabilities survive wrapping (GIGE-MEL-047).** The host wraps
every module state so that it can pin the module. If the module's own state
implements the optional [`IProfileNavigationHandler`](../src/profiles/navigation_handler.hpp),
the loader returns a forwarding wrapper subtype that also implements it.
Otherwise it returns the plain wrapper, which does not. `navigation_handler(state)`
on the host-visible state therefore gives the same answer as on the module
object. The forwarder uses the retained module state's own handler; module code,
the state's control block and deleter retire before the module pin, in the
existing order. The descriptor, discovery symbol and interface revision are
unchanged. The handler header joins the build-identity closure, so pre-047
module binaries are refused as an incompatible build identity and must be
rebuilt with the host. See the [design](tasks/GIGE-MEL-047-design.md#dynamic-modules).

Use `pleora` and its exact MAC ID for the other adapter. Selection never opens
or acquires. Each selected profile is a fresh module factory result. Dropping
the LoadedProfile/catalog does not unload code used by devices or independent
states. [Authoring](camera-profile-authoring.md#matched-c-dynamic-modules-v1-gige-mel-035)
specifies exact compatibility, error and destruction ordering.

The existing positional CLI is preserved:

```sh
./build/035-both-dev/gige_mel_select --profile-module /absolute/profile.so \
  aravis EXACT_DEVICE_ID test_rate_limited_v1
./build/035-both-dev/gige_mel_select --profile-module /absolute/profile.so \
  pleora EXACT_MAC_ID test_rate_limited_v1
```

The optional module argument must precede the existing positional arguments (or
`--list`). Without `--inspect`, this only loads/registers/selects and constructs
a manager. No configuration writes, stream creation or acquisition is implicit.
`--list` still lists compiled backends, not cameras. Built-in/zero-argument
factories and exported MEL signatures remain unchanged. The selection/loader
targets and module implementations are not installed with the common provider.
See the [separate CMake project](../examples/profile_module/README.md) for a real
independent build, required header bundle and matched build settings.
