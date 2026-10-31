# Optional Aravis camera-control foundation (GIGE-MEL-008)

This is the project's first integration with a real camera SDK. It is an
**internal, optional, uninstalled** camera-control layer built on the existing
`ICameraBackend`/`ICameraDevice` contracts. It implements discovery, exact
selection, native ownership, capabilities, configuration and truthful readback.
It does **not** acquire images: `create_stream()` returns
`ErrorCode::unsupported`. It is not an installed or selectable image backend,
and it does not qualify any physical camera.

## Qualified SDK baseline

| Item | Value |
| --- | --- |
| SDK | Aravis **0.8.36** (pkg-config module `aravis-0.8`), LGPL-2.1-or-later |
| Release | <https://github.com/AravisProject/aravis/releases/tag/0.8.36> |
| Release source commit | `97ff5e330919d7a44b871df4eda669feeafdf342` (tag `0.8.36`) |
| Archive | `aravis-0.8.36.tar.xz` from the release assets |
| Archive SHA-256 | `246deaa0042a387ff1bc00332d0fb80537ce14abde2c28d1a54c91f17adc51bf` |

API availability was established from the pinned release source, not from the
rolling online documentation. Other Aravis versions are rejected at configure
time and are untested. Distribution `libaravis-dev` packages are not assumed to
provide 0.8.36. The SDK source is not copied into this repository. It is not
added to `third_party/manifest.json` or `third_party/json-manifest.json`. Aravis
is dynamically linked only by the optional, uninstalled targets. Its license
remains with the upstream project.

## Provisioning versus normal builds

Normal configuration and builds never download anything, and with the default
`GIGE_MEL_BUILD_ARAVIS=OFF` they do not search for Aravis, pkg-config or GLib.
Provisioning is a separate, explicit step:

```sh
scripts/provision_aravis.sh "$HOME/sdk/aravis-0.8.36"        # downloads, or:
scripts/provision_aravis.sh "$HOME/sdk/aravis-0.8.36" --archive aravis-0.8.36.tar.xz
export PKG_CONFIG_PATH="$HOME/sdk/aravis-0.8.36/lib/pkgconfig"
```

The script verifies the SHA-256 before extracting and checks the declared
version. It builds only the `aravis-0.8` shared library with meson, using
`--buildtype release -Dviewer=disabled -Dgst-plugin=disabled -Dusb=disabled
-Dpacket-socket=disabled -Dintrospection=disabled -Ddocumentation=disabled
-Dtests=false`. It does not build or install the upstream command-line programs.
It installs only the library, headers and `.pc` file, and records
`PROVENANCE.txt` in the prefix. Prerequisites are a C compiler, meson ≥ 0.57,
ninja, pkg-config, and GLib ≥ 2.52, libxml2 and zlib development files. The SDK
is **not** sanitizer-instrumented.

## Build option, targets and diagnostic

```sh
cmake --preset dev -B build/aravis-dev -DGIGE_MEL_BUILD_ARAVIS=ON
cmake --build build/aravis-dev --parallel 2
ctest --test-dir build/aravis-dev --output-on-failure
```

With the option ON, a missing or different SDK is a clear configuration error.
The SDK-enabled build adds three CTest entries to the seven default ones. All of
these targets are internal and are not installed or exported:

| Target | Purpose |
| --- | --- |
| `gige_mel_aravis` (static) | `src/backends/aravis/`: SDK mechanics (`aravis_backend.cpp`) and SDK-free profile translation (`camera_profile.cpp`) |
| `gige_mel_aravis_test_support` | `tests/support/aravis_fake_support.*`: test-only Fake interface selection |
| `aravis_adapter_tests` → CTest `aravis_adapter` | Direct adapter scenarios against the real SDK |
| `aravis_embedding_probe` → CTest `aravis_mel_embedding` | Private common-provider configuration probe |
| `gige_mel_aravis_diagnostic` → CTest `aravis_diagnostic_help` | Diagnostic tool (`--help` performs no device I/O) |

```sh
build/aravis-dev/gige_mel_aravis_diagnostic --help
build/aravis-dev/gige_mel_aravis_diagnostic list              # GigE discovery (network)
build/aravis-dev/gige_mel_aravis_diagnostic inspect DEVICE_ID  # read-only
build/aravis-dev/gige_mel_aravis_diagnostic apply DEVICE_ID W H X Y RATE_HZ EXPOSURE_NS
```

`inspect` prints the loaded SDK version, identity strings, native values and
bounds, and the profile decision with precise reasons. It writes no feature and
starts no acquisition. `apply` is separate and explicit. It exits 1 when the
readback differs from the request.

The common provider, the exported factories and the installed package are
unchanged. `getAPI_Manager()` has no Aravis selection, and `src/provider.cpp`
has no backend-name branch.

**Package-isolation enforcement.** The SDK-enabled CI build jobs run
`scripts/verify_package_isolation.py`. The script checks that:

- the adapter tests resolve `libaravis-0.8.so.0` to the provisioned library;
- the common provider loads no Aravis, GLib, GObject, GIO or libxml2 library;
- installed non-documentation text (headers, CMake package metadata) does not
  reference Aravis, `arv.h` or its GLib/pkg-config stack;
- installed binaries contain no Aravis symbols;
- no installed path below the prefix mentions Aravis, except
  `share/doc/…/docs/aravis-integration.md`. The prefix's own name is not
  examined.

It then configures, builds and runs a separate consumer with
`PKG_CONFIG_PATH` unset, and checks that the consumer's cache records no SDK
discovery entries. A detected violation **and** a failure to run a check
(`ldd`/`nm` failure, unreadable file, traversal error) are both fatal. All
checks run, and the process exits nonzero if any failed, so a later successful
consumer build cannot mask an earlier failure.

The original GIGE-MEL-008 step used negated pipelines (`! ldd … | grep …`).
Bash exempts those from `errexit`, so a detected violation did not fail the
step. Its green results are **not** evidence of effective isolation; see the
R1 record in `docs/validation.md`.

## Interface selection: GigE versus test-only Fake

`make_backend()` uses Aravis's GigE Vision interface
(`arv_gv_interface_get_instance()`) only. It has no fallback. Automated tests use
`aravis_fake::make_backend()` from private test support. That selects Aravis's
in-process Fake interface (`arv_fake_interface_get_instance()`, device ID
`Fake_1`): no network scan, no `arv-fake-gv-camera`, and no physical camera.
Both paths use the same adapter implementation. Every feature read and write
still runs through Aravis's GenICam implementation of the Fake camera. No
project camera mock replaces it.

Neither path changes process-wide interface enable flags or calls
`arv_shutdown()`. Factory construction performs no discovery, open or
configuration I/O. The interface singleton is requested lazily on the first
`enumerate()`/`open()`, and the backend then holds its own reference.

## Identity, ownership and synchronization

- **Discovery** returns a copy of each SDK device ID (for GigE,
  `vendor-model-serial`) plus a copied descriptive string. Aravis frees its
  strings at the next refresh, so none are borrowed. An empty list means a
  successful empty enumeration. A missing interface or an entry without an ID
  is an `io_error`. Aravis 0.8.36's `arv_interface_update_device_list()` has no
  error result, so network failures inside GigE discovery are not observable
  as errors.
- **Open** requires a nonempty ID. The adapter refreshes discovery and accepts
  the ID only if it equals exactly one discovered device ID. It then opens
  exactly that ID. `arv_interface_open_device()` would also accept user IDs,
  serials, aliases, MACs or addresses, and a null ID would mean "first camera",
  but neither is ever passed. An unknown ID is `not_found` and nothing else is
  tried. Enumeration indexes are never used as identity.
- **Ownership.** Within one process, only one logically open device may exist
  per (interface, device ID), across all adapter instances. A second open is
  `busy` and never reaches the SDK. The reservation is taken before the native
  open, is exception-safe, and is released on every failed open.
- **Native objects** are held by RAII (`g_object_unref`, `g_error_free`). Each
  device holds its `ArvDevice` reference. Each backend holds its interface
  reference, and a device may outlive its backend.
- **Synchronization.** Discovery and open are serialized per Aravis interface,
  process-wide across adapter instances. Each device serializes its own feature
  I/O, and independent devices do not share a lock. The adapter **cannot** make
  unrelated application calls into Aravis thread-safe. The concurrent-open test
  establishes two things, both observed rather than timed:
  - *Overlap:* the first opener is gated inside `open()`, after its
    reservation and with the interface lock held.
  - *Contention:* a private hook at the locking boundary observes that the
    competitor's real `try_lock` of that interface lock **failed** while the
    first was still mid-open.

  Only then is the first opener released. The test then requires exactly one
  owner, `busy` for the competitor, and a single native open. In production,
  `open()` acquires that lock as before (`try_lock`, then a blocking lock); the
  hook is a private test seam.
- **Logical close** is idempotent. Under the device lock it drops the native
  reference, then releases the reservation. Later `configuration()`, `apply()`
  and `create_stream()` return `disconnected`. `capabilities()` keeps returning
  the snapshot validated at open, without device I/O. Destructors close and do
  not throw.
- **Per-device client policy.** Each opened device enables Aravis range and
  access checking (`arv_device_set_range_check_policy`/`_access_check_policy`).
  These are client-side settings on that `ArvDevice` object, not camera
  features and not process-wide state.

## Supported profile `aravis-mono8-free-running-v1`

| Requirement | Native feature(s) |
| --- | --- |
| Sensor extent (read-only) | `SensorWidth`, `SensorHeight` |
| ROI, read/write, positive 32-bit increment, minimum on its own grid | `Width`, `Height`, `OffsetX`, `OffsetY` (value, min, max, increment) |
| Frame rate, read/write float, Hz | `AcquisitionFrameRate` |
| Exposure, read/write float, µs | `ExposureTime`, else `ExposureTimeAbs` |
| Current format Mono8, entry available | `PixelFormat` |
| Free-running | `TriggerMode` = `Off` when present; `TriggerSelector` = `FrameStart` when present |
| Manual exposure | `ExposureAuto` = `Off` when present |
| Rate control enabled | `AcquisitionFrameRateEnable` = true when present |

If a declared `<Unit>` contradicts Hz or µs, the profile is unsupported. The
profile and its capability snapshot are validated during `open()`. An
unsupported profile fails open with a diagnostic, so no default-initialized
`CameraCapabilities` value is ever returned as a camera fact. The capability
envelope is the integer-Hz range inside the advertised rate bounds and the
ceiling of the minimum exposure. It is not a claim that every integer rate
reads back exactly. A current configuration that cannot be represented does
not block open; `configuration()` reports it explicitly.

The adapter **never** changes the pixel format, trigger state, auto exposure,
gain, binning or any other unrelated feature. It does not use `ArvCamera`
convenience setters. In 0.8.36, `arv_camera_set_frame_rate()` first calls
`arv_camera_clear_triggers()`, which sets every `TriggerMode` to `Off`, and it
clamps the requested value. `arv_camera_set_exposure_time()` can also set
`ExposureMode`. The adapter instead writes each GenICam node directly with
`arv_gc_integer_set_value`/`arv_gc_float_set_value`. Tests verify that
PixelFormat, trigger, AcquisitionMode, GainAuto, gain and binning are
preserved.

On the Fake device (from `arv-fake-camera.xml`), the sensor is 2048×2048 with
unit increments. `AcquisitionFrameRate` is `1e6 / AcquisitionFramePeriod`
(integer µs, 1000–10,000,000) with bounds [0.1, 1000] Hz. `ExposureTimeAbs` is
an integer-µs register with bounds [10, 10,000,000] µs. The initial state is
512×512 at (0,0), 25 Hz, 10 ms, Mono8, with TriggerMode Off.

## Numeric representation and quantization policy

The project stores integer Hz (`uint32_t`) and `std::chrono::nanoseconds`.
Aravis exposes floating-point Hz and µs.

- **Requests.** A rate of 0, or an exposure ≤ 0 or above 2^53 ns, is
  `invalid_argument`. Otherwise the value converts exactly to a double.
- **Readback.** A non-finite or negative native value is `io_error`. A finite
  value `v` converts to the integer `n` in the target unit only if
  `|v − n| ≤ 4 · DBL_EPSILON · max(1, |n|)` and `n` is in range
  (1..2^32−1 Hz; 1..2^63−1 ns). This is a relative epsilon tolerance. It is
  not exactly four adjacent representable doubles ("4 ULP") at every
  magnitude; R1 corrected that earlier wording without changing the formula.
  The tolerance covers floating-point representation error only. Any other
  value is `unsupported`, and the error
  names the feature, the shortest round-trip native value, and the unit. Values
  are never rounded into a successful configuration. Range checks precede
  every integer cast.
- **Real quantization is preserved.** If the readback is representable but
  differs from the request, the actual value is returned. The common provider
  already treats requested ≠ actual as a mismatch.

Pinned 0.8.36 Fake behavior, reproduced by `aravis_adapter`:

| Request | Native state | Readback | Result |
| --- | --- | --- | --- |
| 25 / 40 / 50 / 1 / 1000 Hz | period 40000 / 25000 / 20000 / 1000000 / 1000 µs | exact | equal to the request |
| 30 Hz | period 33333 µs | `30.000300003000032` Hz | `unsupported`: "AcquisitionFrameRate reads back 30.000300003000032 Hz …" |
| (period 142857 set via SDK) | 7 Hz nominal | `7.0000070000069998` Hz | `unsupported` |
| exposure 1,250,500 ns | `ExposureTimeAbs` register 1250 | 1250 µs | success, actual 1,250,000 ns (≠ request) |

A later, exactly representable configuration (for example 40 Hz) restores a
valid readback.

## Transactions, partial application and errors

`apply()` takes the device lock, reads a **fresh** native snapshot, and
requires the supported profile. It validates the **complete** request against
that fresh snapshot before writing anything. All limits come from the fresh
readback, not from the open-time envelope (R1):

1. **Structural safety** (`check_limits`, `check_current`). Every native fact
   used in arithmetic is checked first, on every snapshot; no caller is trusted
   to have validated it. The checks are:
   - sensor extent in 1..2^32−1;
   - per ROI feature: increment in 1..2^32−1, minimum ≤ maximum, and a grid
     origin (the minimum) that is a multiple of the increment and lies in
     [0, sensor] (sizes) or [0, sensor−1] (offsets);
   - rate and exposure bounds finite, nonnegative and ordered, with any
     advertised float increment finite and positive;
   - current ROI geometry inside the sensor: size in [1, sensor], offset in
     [0, sensor − size].

   A violation is `unsupported` (an invalid current float is `io_error`),
   naming the feature and value. Nothing is clamped, wrapped or fabricated.
   The ROI comparisons are written so that no subtraction or sum can overflow
   `int64_t`. Current values are **not** required to be on the grid, inside the
   reported bounds, or representable by the project contract. A fractional
   current rate, or a current exposure above a newly lowered maximum, is still
   reported truthfully and remains recoverable by an explicit `apply()`.
2. **Profile-change detection.** The published `CameraCapabilities` is the
   immutable open-time snapshot. If the fresh sensor extent, any ROI increment,
   or the selected exposure feature (`ExposureTime` versus `ExposureTimeAbs`)
   differs from it, the request is rejected as `unsupported` ("… changed from
   … at open to …; close and reopen the device; nothing was written"). Obsolete
   facts are never used silently. A reopen publishes the fresh facts.
3. **Request validity on fresh limits.** The following are treated as final
   before any write:
   - the ROI minimum and increment grid, a positive size (zero is rejected
     even if a native minimum would allow it), and offset + size ≤ sensor;
   - the frame-rate minimum and maximum;
   - the exposure minimum and advertised float increments;
   - the exposure maximum, except in one case: when the plan lengthens the
     period first. Then the exposure maximum may legitimately rise with that
     rate write, so it is left to the SDK.

   **Supported coupling that is not prevalidated:** a current ROI maximum that
   depends on the other feature of its axis (for example `Width.max` =
   sensor − `OffsetX`). Treating those as final would break the small/far ↔
   large/near transitions. This profile does not model a rate maximum raised by
   an earlier write of the same request; such a request is rejected before any
   write and can be applied in two steps.

Writes go directly to the GenICam nodes in a safe order. On each axis, the
offset is written first if it fits the current size; otherwise the size is
written first. The rate is written first when the period lengthens, and the
exposure first otherwise. Unchanged integers are not rewritten. Aravis range
and access checking stay enabled for constraints that change **after**
prevalidation, and for the coupled maxima above. After the writes, the complete
actual configuration is read back.

If a later write fails, the original SDK error is kept (operation, feature,
GError domain and code). The adapter appends the earlier successful writes,
states that "no rollback was attempted", and adds the actual native
configuration, or why it cannot be read or represented. The native state stays
observable through `configuration()`. Rollback is neither performed nor
claimed. GError translation: GenICam out-of-range or unknown enum entry →
`invalid_argument`; missing node, read-only or undefined property →
`unsupported`; device timeout → `timeout`; not connected → `disconnected`; not
controller → `busy`; anything else → `io_error`.

The partial-write test lowers the Fake's `ExposureTimeAbs` maximum through the
real SDK (`arv_gc_float_impose_max`). It does this from a private hook only
**after** prevalidation, immediately before that write, which models a native
constraint changing mid-transaction. The SDK itself then rejects the write.
This is an arranged SDK constraint change, not a physical camera fault.
Prevalidation is not weakened to force it. Once the reduced maximum is visible
in a fresh snapshot, a retry that violates it fails in prevalidation with zero
further write attempts.

## Optional features: absence versus query failure

`TriggerSelector`, `TriggerMode`, `ExposureAuto`, `AcquisitionFrameRateEnable`
and `ExposureTime` are optional. So are the identity strings shown by
`inspect`. The adapter distinguishes three outcomes of
`arv_gc_feature_node_is_implemented()` (R1):

| Outcome | Treatment |
| --- | --- |
| No node, or the SDK **successfully** reports "not implemented" | Supported optional absence |
| Implemented | Read through the SDK; a node of an unsuitable type is an explicit `unsupported` diagnostic (for example "'ExposureAuto' is not an enumeration or string") |
| The query returns a `GError` | Error: "Aravis query implementation of optional feature 'X': <SDK message> [domain code]" |

A failed query never becomes an empty optional. It never lets a profile check
(for example `TriggerMode`) be skipped, and never falls back from
`ExposureTime` to `ExposureTimeAbs`. A failed open still retires the native
object and releases the ownership reservation. `inspect` reports a failed
identity query as a problem instead of omitting the feature.

## Private MEL embedding evidence

`aravis_mel_embedding` calls `make_api_manager(settings, backend)` with the
unchanged common provider, the upstream `getControl`/`getBuffer`, the Fake
interface, and explicit settings. Instance `aravis-fake-probe` and device
`Fake_1` are used, with **virtual** test optics of 0.0002/0.0003 rad/px. The
probe covers:

- Control init and identity, and the existing Image/C2/Health advertisement;
- C2 and Image `CameraCommand` and `ConfigSet` v1, with correlated status and
  actual geometry;
- Detailed Health backend/device identity;
- quantized exposure → readback mismatch (Rejected);
- 30 Hz → `Unsupported` error with the native value, `configuration_valid`
  false in Health, and explicit recovery;
- Image `enable()` → `Fail` with the adapter's unsupported diagnostic in
  `provider_statistics().last_error`, no image callback, and acquisition state
  idle.

Successful `Control::init()` still advertises the Image channel, because the
common provider has no control-only advertisement. That is why this probe is
private evidence of the configuration paths only. It is **not** a supported
Aravis MEL profile, and the common provider was not changed for it.

## Acquisition is pending

`create_stream()` returns `unsupported` without creating an `ArvStream`,
worker or frame. Before streaming can be implemented, the following are still
needed:

- buffer announce/queue/revoke ownership mapped to `FrameLease` lifetime;
- payload and stride validation, and incomplete/missing-packet handling;
- a stop path that wakes `receive()`;
- disconnection handling;
- a characterized time mapping. Aravis camera timestamps, host receive times
  and receive-minus-exposure estimates must **not** be labelled
  integration-start.

The common provider's frame and timestamp checks stay as they are.

## Qualification boundaries

The evidence covers the project adapter and tests, executed against the real
Aravis 0.8.36 library and its in-process Fake camera. It does **not** establish:

- generic GenICam or GigE Vision compatibility;
- behavior on any physical camera or firmware;
- GVCP/GVSP network behavior, controller privilege, heartbeat or packet
  behavior;
- thread safety of Aravis itself;
- image acquisition or timing.

The first physical-camera qualification and a second real SDK implementation
remain pending. Sanitizer jobs instrument the project's code only, not the
SDK.
