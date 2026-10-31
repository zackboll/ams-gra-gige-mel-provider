# Optional Aravis adapter: camera control (GIGE-MEL-008) and source acquisition (GIGE-MEL-009)

This is the project's first integration with a real camera SDK. It is an
**internal, optional, uninstalled** adapter built on the existing
`ICameraBackend`/`ICameraDevice`/`IFrameStream` contracts. GIGE-MEL-008
implemented discovery, exact selection, native ownership, capabilities,
configuration and truthful readback. GIGE-MEL-009 adds bounded Mono8 **source**
acquisition: `create_stream()`, `IFrameStream`, SDK-buffer-backed `FrameLease`
objects, cancellation, and a direct-capture diagnostic. See
[Source acquisition (GIGE-MEL-009)](#source-acquisition-gige-mel-009).

Every frame carries the default, **invalid** `FrameTimestamp{}`, so the common
MEL provider rejects it: there is no successful Aravis MEL Image publication.
The adapter is not an installed or selectable image backend, and it does not
qualify any physical camera.

> **Superseded status (GIGE-MEL-008 text below).** Passages written for
> GIGE-MEL-008 that say `create_stream()` returns `unsupported`, that Image
> `enable()` fails, or that acquisition is pending describe that milestone.
> They are kept as history and are marked where they appear.

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
The SDK-enabled build adds four CTest entries (three before GIGE-MEL-009) to
the eight default ones. All of these targets are internal and are not
installed or exported:

| Target | Purpose |
| --- | --- |
| `gige_mel_aravis` (static) | `src/backends/aravis/`: SDK mechanics (`aravis_backend.cpp`), SDK-free profile translation and frame validation (`camera_profile.cpp`), and the shared capture routine (`aravis_capture.cpp`) |
| `gige_mel_aravis_test_support` | `tests/support/aravis_fake_support.*`: test-only Fake interface selection |
| `aravis_adapter_tests` → CTest `aravis_adapter` | Direct adapter scenarios against the real SDK |
| `aravis_embedding_probe` → CTest `aravis_mel_embedding` | Private common-provider configuration and negative timestamp-admission probe |
| `aravis_stream_contract_tests` → CTest `aravis_stream_contract` | GIGE-MEL-009 acquisition contract against the real SDK Fake stream |
| `gige_mel_aravis_diagnostic` → CTest `aravis_diagnostic_help` | Diagnostic tool (`--help` performs no device I/O) |

```sh
build/aravis-dev/gige_mel_aravis_diagnostic --help
build/aravis-dev/gige_mel_aravis_diagnostic list              # GigE discovery (network)
build/aravis-dev/gige_mel_aravis_diagnostic inspect DEVICE_ID  # read-only
build/aravis-dev/gige_mel_aravis_diagnostic apply DEVICE_ID W H X Y RATE_HZ EXPOSURE_NS
build/aravis-dev/gige_mel_aravis_diagnostic capture DEVICE_ID OUTPUT.pgm   # one frame (GIGE-MEL-009)
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
  examined;
- every non-documentation symlink resolves to an existing regular file inside
  the prefix, and that file's **content is checked under the link's own
  public-path policy**. A public alias of a file in the excluded documentation
  tree is still scanned (R2). R3 made the following rules precise:
  - **Filesystem-order resolution.** The verifier resolves a link one
    component at a time, using a queue of unresolved components. A symlink in
    any component, directory or final, is expanded before the next component
    is processed. A following `..` therefore applies to the directory that the
    link actually reaches. Relative targets are interpreted in the link's own
    directory. `..` is never simplified lexically (R2's `normpath` was): with
    `alias -> /outside/a/b/c/d`, `alias/../x` is `/outside/a/b/c/x`, not
    the sibling `x` of `alias`.
  - **Containment.** The boundary is the canonical (`realpath`) install
    prefix. A link whose relative `..` would climb above it fails. So does an
    absolute target that does not spell that canonical path component by
    component (`..` is not simplified there either). The rule applies to
    **every** expansion, including links in directory components and links
    under the excluded documentation tree that a public path passes through.
    An excursion outside the prefix fails even if a later link returns inside.
    A clean outside target fails regardless of its content.
  - **Hop accounting.** At most **16 symlink expansions** are followed per
    resolution, counting directory-component and final-component links
    alike: 16 pass, the 17th fails. Cycles, including cycles in directory
    components, fail as overlong chains.
  - **Explicit failures.** Missing targets, cycles, overlong chains, a
    non-directory used as a directory, `lstat`/`readlink`/read errors and
    final targets that are not regular files (including directories and
    FIFOs) are all `installed-links` failures. No traceback or blocking open
    is possible: reads use `O_NOFOLLOW | O_NONBLOCK` and confirm that the
    opened inode is the validated one.
  - **One file throughout.** Containment, the regular-file check, ELF
    classification, version-link eligibility, the text read and `nm` all use
    the one canonical file that resolution produced. Diagnostics and the
    policy still use the public link's path.
  - **Version-link exemption.** A link is skipped, rather than scanned under
    its own name, only when its **actual** target meets all of these
    conditions: it is a regular ELF file; it is in the link's own real
    directory; it is named `<link>.N[.M…]` for a link named
    `lib….so[.N…]`; it lies outside the documentation tree; and the walk
    enumerated it as a regular file, so its own symbol check runs. A clean
    lexical lookalike cannot satisfy this, and neither can a library-named
    file in the documentation tree. The real `libgige_mel_provider.so` →
    `.so.0` → `.so.0.1.0` chain qualifies.
  - **Quiescence.** The install tree is assumed not to change during
    verification. The inode check detects a swapped final file, but this is
    not a concurrent-filesystem security design.

`tests/verify_package_isolation_tests.py` (CTest `package_isolation_verifier`,
which needs no SDK and runs in every CI job) exercises the real verifier CLI
against fixture trees and stub `ldd`/`nm`/`cmake`.

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
- **Logical close** is idempotent. Under the device lock it first retires any
  active acquisition (GIGE-MEL-009, see below), then drops the native
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

1. **Structural safety.** Every native fact used in arithmetic is checked
   before use, and no caller is trusted to have validated it.
   `capabilities_from()` (open, `inspect`) runs `check_preconditions` and
   `check_limits`. `plan_writes()` (every `apply()`) runs those plus
   `check_current`. Open deliberately does **not** run `check_current`, so an
   unusual current state does not block the open that could recover it. The
   checks are:
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
   - each **written** ROI feature's fresh maximum, according to the ordered
     plan (R2; see below);
   - the frame-rate minimum and maximum;
   - the exposure minimum and advertised float increments;
   - the exposure maximum, except in one case: when the plan lengthens the
     period first. Then the exposure maximum may legitimately rise with that
     rate write, so it is left to the SDK.

   **ROI maxima and supported coupling (R2).** A ROI maximum may depend on its
   axis companion; for example `Width.max` = sensor − `OffsetX`. Only a
   **smaller** companion can raise it. A written feature's fresh maximum is
   therefore deferred to the SDK range check **only** when an **earlier write
   of the same plan decreases its companion**. Two cases qualify:
   - the size, when the offset is written first and moves toward 0;
   - the offset, when the size is written first and shrinks.

   In every other case the fresh maximum is final and is enforced before any
   native write, on either axis:
   - the feature is the first write on its axis;
   - the companion is unchanged;
   - the companion increases;
   - the companion is written later.

   So a known-invalid `Height` or `OffsetY` request can no longer let an
   earlier `Width` write through. An unchanged feature is not written, and its
   current value is not judged against a newly lowered maximum. The valid
   small/far → large/near and large/near → small/far transitions still work.
   This profile does not model a rate maximum raised by an earlier write of the
   same request; such a request is rejected before any write and can be applied
   in two steps.

Writes go directly to the GenICam nodes in a safe order. On each axis, the
offset is written first if it fits the current size; otherwise the size is
written first. The rate is written first when the period lengthens, and the
exposure first otherwise. Unchanged integers are not rewritten. Aravis range
and access checking stay enabled for constraints that change **after**
prevalidation, and for the deferred coupled maxima above. After the writes, the
complete actual configuration is read back.

**Float increments (R2).** Aravis 0.8.36's `arv_gc_float_get_inc()` returns
exactly `G_MINDOUBLE` when no increment is defined, and reports failures as a
`GError`. The adapter treats only that exact value, returned without a
`GError`, as "no increment". Any other successfully returned value is kept and
validated: an explicit `<Inc>` of `0`, `-5`, `nan` or `inf` is rejected as
"`<feature>` advertises invalid increment … us" at open, or in an
already-open `apply()` before any write. A query `GError` is still reported as
an error. (Before R2, values ≤ `DBL_MIN` — zero, negatives and NaN — silently
became "no increment".)

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
- *(GIGE-MEL-008, superseded)* Image `enable()` → `Fail` with the adapter's
  unsupported diagnostic. Since GIGE-MEL-009 this endpoint is replaced by the
  negative timestamp-admission scenario described in
  [Negative MEL admission evidence](#negative-mel-admission-evidence).

Successful `Control::init()` still advertises the Image channel, because the
common provider has no control-only advertisement. That is why this probe is
private evidence of the configuration paths only. It is **not** a supported
Aravis MEL profile, and the common provider was not changed for it.

## Acquisition is pending (GIGE-MEL-008 record, superseded)

*Superseded by GIGE-MEL-009 for buffer ownership, payload/stride validation,
incomplete-buffer handling and stop wakeup. Disconnection handling and a
characterized time mapping are still pending.* At GIGE-MEL-008,
`create_stream()` returned `unsupported` without creating an `ArvStream`,
worker or frame. Before streaming could be implemented, the following were
needed:

- buffer announce/queue/revoke ownership mapped to `FrameLease` lifetime;
- payload and stride validation, and incomplete/missing-packet handling;
- a stop path that wakes `receive()`;
- disconnection handling;
- a characterized time mapping. Aravis camera timestamps, host receive times
  and receive-minus-exposure estimates must **not** be labelled
  integration-start.

The common provider's frame and timestamp checks stay as they are.

## Source acquisition (GIGE-MEL-009)

### Acquisition ownership and retirement

Strong references (there are no cycles):

```text
AravisDevice ──┐
AravisStream ──┴─> DeviceCore ──> Run (active / most recent) ──> ArvStream ──> ArvDevice
AravisStream ────> Run (its own most recent)                     (until retirement)
FrameLease owner (LeasedBuffer) ──> Run, and exactly one popped ArvBuffer
```

- A `Run` references no `DeviceCore`, wrapper or lease. A lease deleter
  captures no raw stream, wrapper or device pointer.
- **Retirement** drops `Run::stream`. Outstanding leases then hold only their
  own SDK buffer storage and the small `Run` record. They do not keep the
  native stream, the receiver thread or the device alive.

**Locks.** The order is D before R. W is held only briefly and never while
D or R is acquired.

| Lock | Guards |
| --- | --- |
| `DeviceCore::mutex` (D) | Native feature I/O and every acquisition transition: start, stop/retirement and close. `apply()` takes it. |
| `Run::mutex` (R) | The run's stream pointer, the retired flag, the count of in-flight native waits, and lease accounting. |
| `AravisStream::mutex_` (W) | Only the wrapper's run pointer. |

`receive()` and lease release take only R and never wait for D. `stop()` and
`close()` hold D while they wait for in-flight native waits (at most one
polling slice) and while the SDK finalizer joins its receiver. Neither of
those waits can need D, so they cannot deadlock. No native wait happens while R
is held.

**Native retirement order.** `stop()` and `close()` both run this under D:

1. Set the run's cancel flag. `receive()` returns within one polling slice.
2. Execute `AcquisitionStop` through the SDK. A failure is recorded, never
   hidden (see below).
3. Mark the run retired and take the stream out. Wait until no `receive()` is
   inside a native pop. From now on, a lease release frees its buffer instead
   of requeuing it.
4. Drop the last native-stream reference. That reference is the adapter's own;
   the receive path only borrows the stream, protected by the in-flight count.
   The SDK finalizer then runs synchronously:
   - **Fake:** it joins its own receiver thread
     (`arv_fake_stream_finalize` → `stop_thread`).
   - **GigE:** it stops the receiver and writes the stream-channel register
     (`arv_gv_stream_finalize`) while this device's lock and ownership
     reservation are still held.
   Queued buffers are freed by the finalizer. Leased buffers are freed by their
   lease.

The adapter issues no second explicit thread stop. `close()` then drops the
native device and **only then** releases the reservation. An old finalizer
therefore cannot run after a reopen. `arv_shutdown()` is never called.

### Start, stop, close and restart policy

- **`create_stream()`** returns a lightweight wrapper. It creates no native
  stream, receiver or buffer, writes nothing, and fails with `disconnected`
  after close. The native stream is constructed in `start()`, because the SDK
  stream constructor starts its receiver thread immediately.
- **One acquisition per logical device.** `DeviceCore::active` admits a single
  run, so a repeated or competing `start()`, from any wrapper, fails with
  `busy` and creates no native stream. Several wrappers may exist, but a wrapper
  that never started cannot stop another wrapper's run.
- **Configuration.** `apply()` fails with `invalid_state` before any native
  write while a run is active. Start, stop and retirement hold D, so no
  intermediate state is visible to `apply()`.
- **Each run** re-reads a fresh snapshot, requires the unchanged
  `aravis-mono8-free-running-v1` preconditions and a representable
  configuration, and freezes that configuration and the
  `configuration_generation` passed to `start()`. It gets a new, process-wide
  `stream_generation`, including when the wrapper is new. It also gets a
  **fresh native stream and pool**, so stale buffers from an earlier run cannot
  be relabelled.
- **Start prerequisites.** `AcquisitionMode` must already be `Continuous`.
  `AcquisitionStart` and `AcquisitionStop` must be implemented, available,
  writable GenICam commands, and `PayloadSize` must be readable. Anything else
  is rejected explicitly. The adapter never changes trigger mode, automatic
  controls, pixel format, frame rate, exposure or gain to make streaming work.
- **Restart** on the same open device is refused with `resource_exhausted`
  while leases from the previous run are outstanding. This applies through a
  replacement wrapper too. Once they are released, an explicit `start()` works.
  Configuration may change after a completed stop. Old lease descriptors and
  bytes stay unchanged.
- **Reopen.** A separately reopened device may start once the old device's
  native work is retired. Releasing an old lease then frees its buffer and
  never touches the new acquisition.
- **`request_stop()`** (noexcept) sets the cancel flag only. **`stop()`**
  (noexcept) retires the wrapper's active run. It is safe before start, after a
  failed start, after a normal stop, and when repeated.
- **`close()`** wakes `receive()`, which then returns `disconnected`. It retires
  native work and blocks later device operations. The existing caller contract
  still applies: join the receiving thread before destroying a stream wrapper.
- **Start failure after partial setup.** `AcquisitionStart` is the last native
  step. If it fails (or allocation fails), the adapter cancels, retires and
  drops the new native stream; its finalizer stops the receiver and frees every
  queued buffer. Nothing is published, no acquisition owner remains, and
  configuration is unchanged. The error keeps the SDK operation context and
  says to retry `start()` explicitly. `AcquisitionStop` is not sent for a start
  that did not succeed.
- **Stop-command failure.** Local resources are still retired. The first
  failure, with its operation, feature and generation context, is kept and
  returned once by `close()`: "… local stream resources were retired anyway;
  the camera may still be acquiring". The diagnostic reports it as a cleanup
  failure. The camera is never claimed to have stopped.

**Command evidence.** Starting and stopping are verified independently of
frame arrival. The Fake receiver produces frames whether or not acquisition
was commanded, so a frame proves nothing about the commands. The tests read the
Fake acquisition register (address `0x124`, written only by
`AcquisitionStart`/`AcquisitionStop`) through the SDK's public
`arv_fake_camera_get_acquisition_status()`.

### Pool limits and statistics

| Item | Value |
| --- | --- |
| Buffers per run | 4 (`aravis_profile::pool_buffers`), allocated with `arv_buffer_new_allocate()` before `AcquisitionStart`; never per frame |
| Buffer size | The `PayloadSize` readback; it must be positive and at least width × height |
| Per-buffer limit | 64 MiB |
| Pool limit | 256 MiB |
| Arithmetic | Every conversion and product is overflow-checked; violations fail before allocation (`unsupported` or `resource_exhausted`) |

Pool exhaustion never allocates more: `receive()` times out.

Statistics belong to each wrapper and **accumulate across that wrapper's
runs**. Snapshots are atomic loads, safe during `receive()` and shutdown.

| Counter | Meaning |
| --- | --- |
| `delivered_frames` | Successful leases returned |
| `incomplete_frames` | Popped native buffers with a non-success completion status (timeout, missing or wrong packets, aborted). They are requeued and never published. |
| `timeout_count` | Expired **public** `receive()` calls, not internal polling slices |
| `pool_starvation_events` | Episodes in which a native wait slice found nothing while **every** pool buffer was held by a lease for the whole slice. Counted once per episode and cleared by a lease release. An arbitrary SDK underrun or empty queue is not counted. |

### Receive deadline and cancellation

`receive(timeout)` computes **one** overall steady-clock deadline, saturating
instead of overflowing for huge timeouts.

- A negative timeout returns `invalid_argument`.
- A zero timeout performs exactly one non-blocking `arv_stream_try_pop_buffer()`
  and no native wait.
- Otherwise each native wait is `arv_stream_timeout_pop_buffer()` for at most
  **5 ms**. The SDK unit is microseconds, and the conversion is checked. The
  cancel flag is checked before every slice, so `request_stop()`, `stop()` and
  `close()` end a genuinely blocked receive within about one slice.
- The deadline is never restarted after a slice or after a discarded buffer.
- A null pop is a transient wait result. It is **never** reported as
  disconnection or packet loss. Physical transport recovery is out of scope.
- A receive that popped a valid buffer before retirement may still return its
  lease, as `backend.hpp` allows. The provider gates publication itself.
- Before start, `receive()` returns `stopped`. After `stop()`/`request_stop()`
  it returns `stopped`, and after `close()` it returns `disconnected`.

### Supported payload and explicit exclusions

The SDK's completion status is checked **before** any accessor whose
precondition is a successful image, so no GLib critical is provoked. The test
executable makes GLib warnings and criticals fatal. A buffer is accepted only
when it is a complete, chunk-free, single-part 2-D image with:

- PFNC `Mono8` (`0x01080001`): one band, 8 storage and 8 significant bits;
- byte order `not_applicable`, because the samples are single bytes;
- width, height, `OffsetX` and `OffsetY` equal to the frozen configuration;
- no Y padding, and X padding represented as `row_stride = width + x_padding`;
- an image part lying inside the SDK-reported received size and the
  allocation;
- an SDK-reported image size of at least `row_stride × height`.

The leased span is exactly `row_stride × height` bytes of SDK-reported valid
data. Allocation capacity is never used as evidence, and final-row padding is
never fabricated.

The following are not delivered:

| Buffer | Treatment |
| --- | --- |
| **Incomplete** (non-success status) | Counted, requeued, and retried within the same deadline |
| **Malformed complete:** another status, chunk, extended-chunk, multipart or non-image payloads, packed or other pixel formats, a geometry mismatch, Y padding, inconsistent sizes, arithmetic overflow | Requeued, not counted as incomplete, and retried within the deadline. If the deadline expires, the `timeout` error names the last rejection reason. |

`native_frame_id` is the SDK value, unmodified. It is not assumed to be
globally monotonic or non-wrapping; the Fake ID is 16-bit and skips 0. It is
kept distinct from the stream and configuration generations.

### Direct capture

```sh
build/aravis-dev/gige_mel_aravis_diagnostic capture DEVICE_ID OUTPUT.pgm
```

The command validates its arguments before any device I/O. It then:

1. opens exactly the discovered `DEVICE_ID` on the production GigE-only
   backend, with no Fake fallback;
2. reads the current supported, representable configuration, writing nothing;
3. starts one acquisition and waits up to **5 s** (one deadline) for one
   validated Mono8 frame;
4. writes a tightly packed binary PGM (`P5`, maxval 255, rows copied without
   stride padding);
5. stops and closes.

Cleanup runs on start, receive, validation and output failures. A failed
`AcquisitionStop` is reported as a cleanup failure, and the exit status is 1.
The command reuses `aravis::capture_one()` (`aravis_capture.cpp`), the same
routine the tests drive with the private Fake backend. Example output from that
routine, as printed by the diagnostic (Fake defaults; no physical camera was
used):

```text
Aravis runtime 0.8.36, compiled against 0.8.36
Captured device Fake_1: 512x512 Mono8, source row stride 512 bytes, native frame ID 65401, stream generation 1
  timestamp: unqualified (FrameTimestamp invalid; no camera, UTC/PTP or integration-start claim)
  configuration:
  width=512 height=512 offset_x=0 offset_y=0 frame_rate_hz=25 exposure_ns=10000000
  statistics: delivered 1, incomplete 0, timeouts 0, pool starvation 0
  wrote OUTPUT.pgm (262159 bytes, tightly packed P5)
```

Generated images are not committed.

### Unqualified timestamps and negative MEL admission evidence

<a id="negative-mel-admission-evidence"></a>Every returned frame uses
`FrameTimestamp{}`: `valid == false`, with domain and event `unspecified`.
No clock, epoch or integration-start mapping is claimed. Specifically, the
adapter does not do any of the following:

- relabel the Fake fill time (host wall clock) as integration start;
- relabel host time as `host_monotonic`;
- treat a GigE fallback timestamp as camera time;
- subtract exposure from a receive time;
- infer UTC or PTP from a number.

`backend.hpp` is unchanged. Raw SDK timestamps are neither exposed nor printed.

The common provider accepts only valid integration-start timestamps in its
declared domain. It therefore **correctly rejects** every Aravis frame. Its
admission code is unchanged. `aravis_mel_embedding` proves this as **negative**
evidence:

- Destination buffers are filled with the sentinel `0xA5` and registered.
  Image `enable()` succeeds, and the real SDK `AcquisitionStart` runs.
- A test-only observing decorator records the frames the adapter actually
  returns. Every one meets all non-time admission conditions (layout,
  configuration and generation, Mono8, complete, span) and carries exactly the
  default invalid timestamp.
- `provider_statistics().rejected_frames` increases with them.
- There are zero Image callbacks, the destination bytes are unchanged, and no
  stream or callback failure occurs. Health shows the acquisition as `running`
  (owned, not faulted). Configuration is rejected while it is owned.
- After explicit `disable()`, configuration works again, the buffers
  unregister and re-register, and a second enable/disable cycle again yields
  only rejected frames.

Health state rules were not changed. This is **not** successful MEL image
publication, and Aravis is not added to the shared positive MEL scenarios.

### Fake-device evidence and its limits

`aravis_stream_contract` runs these scenarios against real `ArvStream`
receiver threads and SDK buffers:

| Group | Behavior |
| --- | --- |
| A | Commands and a single acquisition owner |
| B | Every pixel checked against `(x + y + native_frame_id) % 255` at 10 000 µs exposure and gain 0, a setup verified by readback |
| C | Pool exhaustion and release |
| D | Cancellation of a receive proven blocked in native waits, via `request_stop`, `stop` and `close`, three times each |
| E | Lease lifetime beyond stop, close and owner destruction; release on another thread; reopen |
| F | Restart policy |
| G | SDK-arranged command failures |
| H | Real SDK arrangements, allocation limits, and the labelled decoded-facts seam (see below) |
| I | Exception unwind while receive is blocked |
| J | The shared capture routine, with an independent PGM check |

**Decoded-facts seam.** The `TestHooks::decoded_facts` seam edits only the
*facts* decoded from real popped buffers before the production validator sees
them. The pinned Fake cannot emit incomplete, chunked, multipart, padded or
short buffers, so this is how those payloads are tested. Positive streaming
never uses a project-generated frame.

**Limits.** The Fake stream is in-process. It does not exercise GVSP packets,
resend, packet loss, socket buffers, controller privilege, the GigE finalizer's
register write, disconnection, real exposure timing or throughput.

### SDK versus project sanitizer coverage

The sanitizer lanes instrument the project's adapter, provider and tests only.
Aravis 0.8.36, GLib and libxml2 are **not** instrumented, so project sanitizer
coverage does not imply instrumented SDK coverage.

Under TSan this matters for one reason. On Linux, GLib's `g_mutex_lock`, used
by Aravis's `GAsyncQueue` buffer handoff, is an inline futex rather than
`pthread_mutex_lock`. TSan cannot see that synchronization. It does see
`malloc`/`free`/`memset` called from those libraries, so it reports the SDK's
correctly locked handoffs (`g_queue_pop_tail`, `arv_buffer_set_n_parts`) as
races.

The two SDK-driving test executables link
`tests/support/tsan_uninstrumented_sdk.cpp`. Its
`__tsan_default_suppressions()` lists exactly those two SDK functions.
`TSAN_OPTIONS`, presets and CI lanes are unchanged, and accesses by the
project's own code, including reads of leased pixels, remain checked with
`halt_on_error=1`.

## Qualification boundaries

The evidence covers the project adapter and tests, executed against the real
Aravis 0.8.36 library and its in-process Fake camera. It does **not** establish:

- generic GenICam or GigE Vision compatibility;
- behavior on any physical camera or firmware;
- GVCP/GVSP network behavior, controller privilege, heartbeat or packet
  behavior;
- thread safety of Aravis itself;
- camera timing, timestamps, PTP/UTC, transport (GVSP) acquisition,
  disconnection recovery or hardware performance. GIGE-MEL-009 source
  acquisition is exercised only with the SDK's in-process Fake stream.

The first physical-camera qualification and a second real SDK implementation
remain pending. Sanitizer jobs instrument the project's code only, not the
SDK.
