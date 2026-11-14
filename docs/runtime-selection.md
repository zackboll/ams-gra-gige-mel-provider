# Internal runtime backend and profile selection (GIGE-MEL-033)

## Scope and API

This is build-local C++20 composition, not a new MEL consumer interface, public
provider ABI, stable binary plugin contract or dynamic-library loader. Only one
canonical profile binding exists: `mono_free_running_v1`. It selects the existing
monochrome/free-running policy; it does **not** rename the historical diagnostic
identifier `aravis-mono8-free-running-v1`, select a new pixel format, qualify
camera clocks or advertise new MEL capabilities.

The explicitly owned `selection::BackendCatalog` in
`src/selection/backend_catalog.hpp` knows only the SDK-free `ICameraBackend`
contract. Registration supplies an exact backend ID, the supported canonical
profile and a constructor. It rejects empty/NUL-containing IDs, missing callable
constructors, duplicate IDs and unsupported profiles. Creation invokes only the
selected constructor, preserves returned errors, translates exceptions, rejects
null results and validates the returned backend ID. There is no fallback.
Registration does not invoke constructors. Listing IDs does not invoke them
either. Compose/register before sharing a catalog; concurrent mutation is not
supported. Custom constructors must provide independent backends and perform no
device I/O; the generic catalog cannot police arbitrary user callbacks.

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

Both real adapters still compile/link the **same unchanged**
`gige_mel_camera_profiles` implementation from
`src/profiles/mono_free_running.{hpp,cpp}` and
`src/profiles/mono_free_running_device.{hpp,cpp}`. That code owns aliases, units,
configuration constraints, ordered writes, actual readback, pixel validation and
frame admission. The synthetic backend implements its existing regression
semantics; it does not pretend to have native feature nodes or a physical camera.

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

Multiple independently selectable profiles still require explicit profile
implementations, compatibility/capability negotiation and per-adapter composition
tests. Dynamic profiles additionally need a separately reviewed versioned ABI,
loading/unloading, ownership and mismatch contracts. Neither exists here. No
physical camera tests run implicitly; no new SDK/camera combination or production
timestamp is qualified by these offline checks.
