# Optional Aravis adapter: camera control (GIGE-MEL-008), source acquisition (GIGE-MEL-009) and timestamp provenance (GIGE-MEL-010)

This is the project's first integration with a real camera SDK. It is an
**internal, optional, uninstalled** adapter built on the existing
`ICameraBackend`/`ICameraDevice`/`IFrameStream` contracts. GIGE-MEL-008
implemented discovery, exact selection, native ownership, capabilities,
configuration and truthful readback. GIGE-MEL-009 adds bounded Mono8 **source**
acquisition: `create_stream()`, `IFrameStream`, SDK-buffer-backed `FrameLease`
objects, cancellation, and a direct-capture diagnostic. See
[Source acquisition (GIGE-MEL-009)](#source-acquisition-gige-mel-009).

Every frame carries the default, **invalid** `FrameTimestamp{}`, so the common
MEL provider rejects it: there is no production Aravis MEL Image publication.
The adapter is not an installed or selectable image backend, and it does not
qualify any physical camera.

GIGE-MEL-010 adds two things:

- **Read-only observation** of the raw SDK timestamps.
- A **test-only virtual-clock** fixture. With it, the same genuine SDK pixels
  reach real MEL Image callbacks. This is test qualification of the
  publication path, not timestamp qualification.

See [Native timestamp provenance (GIGE-MEL-010)](#native-timestamp-provenance-gige-mel-010).

GIGE-MEL-011 runs the **unchanged production** `make_backend()` (Aravis's GigE
Vision interface) against the pinned SDK's own network camera simulator,
`ArvGvFakeCamera`, on IPv4 loopback. That exercises real GigE discovery, GVCP
control and GVSP image transfer over UDP. It is simulated network transport,
not physical-camera, NIC or timing qualification. The client's traffic is
loopback-only. The simulator, however, also binds a wildcard
255.255.255.255:3956 discovery listener, so the test runs under a documented
test-only network exception; owner acceptance is pending. See
[Loopback GigE Vision transport (GIGE-MEL-011)](#loopback-gige-vision-transport-gige-mel-011)
and [Network exposure](#network-exposure-of-the-simulator-gige-mel-011-r1).

| Evidence | What it is | What it is not |
| --- | --- | --- |
| In-process Aravis Fake (`ArvFakeInterface`/`ArvFakeStream`) | Completed software tests (GIGE-MEL-008 to -010) | No GVCP, no GVSP, no sockets |
| Loopback `ArvGvFakeCamera` (GIGE-MEL-011) | Simulated network camera; actual GigE Vision discovery, GVCP control and GVSP image transport over UDP on 127.0.0.1 | No physical NIC, switch or camera qualification |
| Physical GigE camera | **Not yet qualified** | — |
| Timestamps | Raw SDK values observed; production descriptors remain invalid (`FrameTimestamp{}`); virtual time used only by tests | No integration-start, PTP or UTC claim |

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
The SDK-enabled build adds six CTest entries to the eight default ones (14 in
total; five before GIGE-MEL-011, four before GIGE-MEL-010, three before
GIGE-MEL-009). All of these targets are internal and are not installed or
exported:

| Target | Purpose |
| --- | --- |
| `gige_mel_aravis` (static) | `src/backends/aravis/`: SDK mechanics (`aravis_backend.cpp`), SDK-free profile translation and frame validation (`camera_profile.cpp`), and the shared capture (`aravis_capture.cpp`) and time-probe (`aravis_time_probe.cpp`) routines |
| `gige_mel_aravis_test_support` | `tests/support/aravis_fake_support.*`: test-only Fake interface selection |
| `aravis_adapter_tests` → CTest `aravis_adapter` | Direct adapter scenarios against the real SDK |
| `aravis_embedding_probe` → CTest `aravis_mel_embedding` | Private common-provider configuration and negative timestamp-admission probe |
| `aravis_stream_contract_tests` → CTest `aravis_stream_contract` | GIGE-MEL-009 acquisition contract against the real SDK Fake stream |
| `aravis_timestamp_contract_tests` → CTest `aravis_timestamp_contract` | GIGE-MEL-010: native timestamp observation, the time-probe routine against the Fake, and positive/negative MEL publication through the **test-only** virtual clock (`tests/support/aravis_virtual_clock.*`) |
| `aravis_gige_loopback_contract_tests` → CTest `aravis_gige_loopback_contract` | GIGE-MEL-011: the production GigE backend against the SDK's `ArvGvFakeCamera` (GVCP/GVSP on 127.0.0.1; real discovery, GVCP, GVSP), plus the R1 socket-audit control case. Test-only fixture `tests/support/aravis_gv_loopback.*`. Needs IPv4 loopback with UDP 3956 free; never skipped. **While it runs, the simulator holds a wildcard 255.255.255.255:3956 listener** ([test-only exception](#network-exposure-of-the-simulator-gige-mel-011-r1)) |
| `gige_mel_aravis_diagnostic` → CTest `aravis_diagnostic_help` | Diagnostic tool (`--help` performs no device I/O) |

```sh
build/aravis-dev/gige_mel_aravis_diagnostic --help
build/aravis-dev/gige_mel_aravis_diagnostic list              # GigE discovery (network)
build/aravis-dev/gige_mel_aravis_diagnostic inspect DEVICE_ID  # read-only
build/aravis-dev/gige_mel_aravis_diagnostic apply DEVICE_ID W H X Y RATE_HZ EXPOSURE_NS
build/aravis-dev/gige_mel_aravis_diagnostic capture DEVICE_ID OUTPUT.pgm   # one frame (GIGE-MEL-009)
build/aravis-dev/gige_mel_aravis_diagnostic time-probe DEVICE_ID COUNT OUTPUT.jsonl  # raw SDK timestamps (GIGE-MEL-010)
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

GIGE-MEL-011 adds a third test-only arrangement that does **not** change the
client: the loopback test uses the production `make_backend()` itself, and the
private fixture only starts the SDK's network simulator and restricts the
client's GigE discovery to the loopback interface for that test process. The
simulator itself is **not** loopback-confined: it also holds a wildcard
discovery listener (see
[Network exposure](#network-exposure-of-the-simulator-gige-mel-011-r1)).

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
  write while a run is active, or while an acquisition-command outcome is
  uncertain (R1). Start, stop and retirement hold D, so no intermediate state
  is visible to `apply()`.
- **Each run** re-reads a fresh snapshot, passes the read-only acquisition
  preflight (R1: preconditions, structural limits, current ROI geometry, the
  open-envelope comparison and representability), and freezes that configuration and the
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
- **Start and stop failures.** See
  [Acquisition-command outcome and recovery (GIGE-MEL-009-R1)](#acquisition-command-outcome-and-recovery-gige-mel-009-r1).
  *(Superseded GIGE-MEL-009 statements, kept as history: "`AcquisitionStop`
  is not sent for a start that did not succeed", and "a failed stop is only
  reported by `close()`" with configuration and restart then permitted.
  Both treated receiver retirement as if it settled the camera command.)*

### Acquisition-command outcome and recovery (GIGE-MEL-009-R1)

The adapter keeps three **private** concepts apart:

| Concept | Meaning |
| --- | --- |
| Receiver ownership (`DeviceCore::active`) | A run owns a native stream and its receiver |
| Camera-command outcome (`none` / `owned` / `uncertain`, plus the owning wrapper's token) | What the adapter knows about the result of **its own** `AcquisitionStart`/`AcquisitionStop` |
| Historical diagnostic | The first unrecovered failure of the void `stop()` interface, kept for one report by `close()` |

These are adapter command/lifecycle states. They are not measurements of
camera exposure or readout completion. **Retiring the native receiver never
establishes that `AcquisitionStop` succeeded.** `apply()` and `start()` are
gated on the current outcome, not on whether a historical error was retained.

- **Uncertainty is recorded first.** `execute_command()` sets `uncertain`
  immediately before calling the SDK command executor. The store is plain and
  allocation-free, so the state survives an exception while the diagnostic is
  being built. Only a successful command settles the outcome.
- **Failure before execution** (preflight, preparation, `PayloadSize`, pool
  planning, stream creation, buffer allocation, a missing or unwritable
  command): no command was attempted. The partial stream and pool are retired,
  no uncertainty is invented, and explicit retry or configuration remains
  available.
- **Failure after `AcquisitionStart` was attempted.** No translated SDK error
  is treated as proof that the command was not applied. The original start
  error is kept, and **one** compensating `AcquisitionStop` is attempted while
  the device and its reservation are held. The partial stream and pool are
  retired whatever happens.
  - If the stop succeeds, the result is still the start failure, now reporting
    "a compensating AcquisitionStop succeeded". Later explicit use is
    permitted.
  - If it fails, the outcome stays `uncertain` and this wrapper owns it. The
    error contains both failures. `apply()` is refused before any write, and
    `start()` before any stream, pool or command. Read-only `configuration()`
    and `capabilities()` remain available.
  - `AcquisitionStart` is never retried automatically.
- **Failed stop.** `stop()` still retires the receiver, and outstanding leases
  stay valid. The outcome becomes `uncertain`. Configuration and new
  acquisition are refused with `invalid_state`, quoting the failure.
- **Explicit recovery** goes through the existing `stop()`. A later `stop()`
  from the **owning** wrapper, made after its native stream is gone, issues
  one bounded `AcquisitionStop` with no internal retry loop.
  - Success clears the uncertainty; failure keeps it.
  - `request_stop()` remains prompt, non-blocking cancellation signalling and
    sends no command.
  - Recovery does not bypass the restart lease rule: a start is still refused
    while the previous run's leases are held.
- **Recovery ownership.** Each wrapper has a stable private numeric token: a
  never-reused process counter, so there is no pointer, cycle or shared
  interface.
  - A never-started or obsolete wrapper, and an old wrapper's destructor,
    cannot stop another wrapper's run or recover its fault.
  - A failed start, which never publishes a run, still records its wrapper as
    owner, so that wrapper's later `stop()` is not a no-op.
  - The destructor makes the same single attempt as `stop()`. If that attempt
    fails, the uncertainty is not cleared or handed to another wrapper.
- **`close()`** has authority over the device's outstanding state.
  - With an active run, it retires the run as before.
  - With no native stream but an `owned` or `uncertain` outcome, it still
    attempts one `AcquisitionStop`.
  - It then drops the native device and only then the reservation, and closes
    logically whatever the command outcome.
  - It returns the unreported historical failure, if any. If a later stop
    succeeded, it marks that failure "recovered: … succeeded"; otherwise it
    adds its own failure and "the camera acquisition outcome remains
    uncertain".
  - A start error keeps the primary start failure as well as the cleanup
    failure.
  - A repeated close performs no device I/O.
  - Close or reopen alone is **not** evidence that a failed camera stop was
    resolved. There is no cross-session recovery registry and no
    hardware-state qualification.
- **Fresh acquisition preflight.** Under the device lock, before any stream,
  allocation or command, `start()` calls the SDK-free
  `aravis_profile::acquisition_preflight(fresh, envelope)`. It applies the
  existing checks in order:
  - profile preconditions;
  - structural native limits;
  - current ROI geometry inside the sensor;
  - the same open-envelope comparison and "close and reopen" diagnostic that
    `apply()` uses, for the sensor extent, the four ROI increments and the
    exposure feature;
  - representability.

  It returns the configuration to freeze. It is read-only: it does not run
  `apply()` or write planning, and it adds no write-ordering or
  coupled-maximum rule. Dynamic maxima and scalar bounds stay governed by the
  existing profile rules. A current value need not lie on the advertised grid.
  `configuration_from()` remains representability-only.

**Command evidence.** Starting and stopping are verified independently of
frame arrival. The Fake receiver produces frames whether or not acquisition
was commanded, so a frame proves nothing about the commands. The tests read the
Fake acquisition register (address `0x124`, written only by
`AcquisitionStart`/`AcquisitionStop`) through the SDK's public
`arv_fake_camera_get_acquisition_status()`.

### Pool limits and statistics

| Item | Value |
| --- | --- |
| Buffers per run | 4 (`aravis_profile::pool_buffers`), allocated with `arv_buffer_new_allocate()` before `AcquisitionStart`. No additional pixel buffers are allocated after pool setup; each delivered lease still allocates its small shared owner record (R1 wording correction) |
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

`backend.hpp` is unchanged. *(GIGE-MEL-010: raw SDK timestamps are now
observable through the private adapter function
`receive_with_native_timestamps()` and the `time-probe` diagnostic. They are
still never placed in `FrameDescriptor::timestamp`.)*

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
| D | Cancellation of a receive proven blocked in native waits, via `request_stop`, `stop` and `close`, and (R1) the shared cancellation-first failure cleanup after a throw, three times each |
| E | Lease lifetime beyond stop, close and owner destruction; release on another thread; reopen |
| F | Restart policy |
| G | SDK-arranged command failures; R1: failed stop blocks configuration and restart (lease released first), explicit `stop()` recovery |
| H | Real SDK arrangements, allocation limits, and the labelled decoded-facts seam (see below) |
| I | Exception unwind while receive is blocked |
| J | The shared capture routine, with an independent PGM check |
| K | R1: start outcomes — known pre-execution failure; applied start + **injected command-result fault** with compensating stop that succeeds / that the SDK rejects; recovery without a native stream |
| L | R1: uncertain outcome then `close()` with no native stream, final stop succeeding and failing |
| M | R1: recovery ownership across wrappers and generations, owner destruction, held-lease rule |
| N | R1: fresh acquisition preflight on real SDK arrangements, zero side-effect deltas |

**Evidence types (R1).** Each case is one of the following:

- **Real Fake-device execution.**
- **SDK-arranged condition.** The SDK itself rejects a command (an imposed
  GenICam access mode) or reports a value written or arranged through its
  public API, and the test reads that value back before relying on it.
- **Injected command-result fault.** `TestHooks::command_result` runs only
  after the SDK executed the command **successfully**, and the test proves
  the Fake acquisition register changed. It then substitutes an error. That
  models an uncertain native command result after a genuine effect. Recovery
  decisions remain in production code. It is **not** packet-loss or
  lost-acknowledgment qualification.
- **SDK-free profile tables** (`aravis_adapter`).

**Decoded-facts seam.** The `TestHooks::decoded_facts` seam edits only the
*facts* decoded from real popped buffers before the production validator sees
them. The pinned Fake cannot emit incomplete, chunked, multipart, padded or
short buffers, so this is how those payloads are tested. Positive streaming
never uses a project-generated frame.

**Limits.** The Fake stream is in-process. It does not exercise GVSP packets,
resend, packet loss, socket buffers, controller privilege, the GigE finalizer's
register write, disconnection, real exposure timing or throughput.
GIGE-MEL-011 exercises GVSP packets, controller privilege, the GigE finalizer's
stream-channel write and simulated loss against the SDK's loopback simulator;
resend, physical loss, disconnection, exposure timing and throughput remain
unqualified.

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

The SDK-driving test executables link
`tests/support/tsan_uninstrumented_sdk.cpp`. Two did at GIGE-MEL-009; since
GIGE-MEL-010 there are three, with `aravis_timestamp_contract_tests` added.
The same file and the same two patterns are used, so the reporting blind spot
below now also applies to that executable. Its
`__tsan_default_suppressions()` lists exactly those two patterns:
`race:g_queue_pop_tail` and `race:arv_buffer_set_n_parts`. `TSAN_OPTIONS`,
presets and CI lanes are unchanged.

**What the patterns cost (R1 correction).** A `race:` pattern suppresses the
**whole report** whenever the named function appears in any frame of either
racing stack. Project code stays instrumented. Even so, a report in which
project code participates is also suppressed when the other access passes
through one of these GLib/Aravis frames. This is a specific reporting blind
spot at the SDK buffer-handoff ownership boundary. A green TSan run with these
patterns is therefore **not** unrestricted race coverage of that boundary.
Reports that match neither pattern still halt with `halt_on_error=1`.

The review's intentional-race control demonstrated these suppression
semantics; it did not discover a production adapter race. The basis for the
patterns, the unsuppressed reports and the analysis behind them, is kept in
the GIGE-MEL-009 and GIGE-MEL-009-R1 records in
[validation](validation.md#gige-mel-009-r1-review-corrections-2026-11-01).

*Superseded (GIGE-MEL-009):* "accesses by the project's own code … remain
checked" overstated this. Project code is instrumented, but reports that
involve these frames are not delivered.

**GIGE-MEL-011 (loopback executable only).** The three earlier executables and
`tests/support/tsan_uninstrumented_sdk.cpp` are unchanged. The new
`aravis_gige_loopback_contract_tests` links its own
`tests/support/tsan_gige_loopback_sdk.cpp` instead. It carries the same two
patterns plus `called_from_lib:` entries for `libaravis-0.8.so.0`,
`libglib-2.0.so.0`, `libgio-2.0.so.0` and `libgobject-2.0.so.0`. The GigE path
adds two SDK threads (the `ArvGvDevice` heartbeat and the `ArvGvStream`
receiver). With only the two existing patterns, one full run of this
executable produced **68** reports (`halt_on_error=0`). Each had an
intercepted libc call made from inside those uninstrumented libraries on at
least one side:

- 20 are `recv`/`poll` on the GVCP socket buffer under `io_data->mutex`, a
  GMutex. They are SDK-internal on both sides.
- 35 are `eventfd`/`write`/`close` on GCancellable descriptors during SDK
  finalization. They are SDK-internal on both sides.
- 13 are the **SDK buffer-handoff boundary**. The `ArvGvStream` receiver's
  `memcpy` of GVSP payload into the pool buffer is reported against a
  **project** read of the leased bytes after `arv_stream_timeout_pop_buffer()`:
  the test's pixel checks, `write_pgm`, and the provider's destination copy.
  This is the boundary the two GIGE-MEL-009 patterns already cover for the
  Fake stream. On GigE the writer frame is `memcpy`, so those patterns do not
  match.

`called_from_lib` makes TSan ignore the accesses and synchronization of
intercepted calls made from those libraries. It does not suppress reports by
stack frame. Its blind spot is **wider** than the two patterns, and it applies
to this executable only. No race in which one side is such an SDK-internal
libc access is reported, including the SDK writing a buffer the project
reads. A green TSan result for this executable is therefore not race coverage
of the buffer-handoff boundary. That boundary is asserted functionally
instead: held-lease bytes stay unchanged after stop, while held, and across
reopen. An intentional project-only race still halts with this file linked.
The full report analysis is in
[validation](validation.md#gige-mel-011-loopback-gige-vision-transport-2026-11-02).

## Native timestamp provenance (GIGE-MEL-010)

GIGE-MEL-010 **observes** the SDK's timestamps and exercises positive MEL Image
publication of genuine Aravis pixels under a **test-only virtual clock**. It
qualifies no camera timestamp. Every production Aravis frame still carries
`FrameTimestamp{}`, and `src/provider.cpp` is unchanged.

| Milestone | Status |
| --- | --- |
| Native Mono8 source acquisition | Implemented, Fake-tested (GIGE-MEL-009) |
| Raw SDK timestamp observation | **Implemented** (read-only adapter path and `time-probe`) |
| Positive Aravis-pixel MEL publication under virtual test time | **Test-qualified** (`aravis_timestamp_contract`, test-only clock) |
| Real Aravis integration-start timestamp | **Not qualified** |
| Production Aravis-backed MEL Image publication | **Not enabled** |
| Physical-camera qualification | **Not performed** |
| Simulated GVCP/GVSP transport (loopback `ArvGvFakeCamera`) | **Exercised** in GIGE-MEL-011 (`aravis_gige_loopback_contract`) |

### What the pinned SDK actually reports

The citations below are to the pinned release commit
`97ff5e330919d7a44b871df4eda669feeafdf342`. All nine cited files in the
checksum-verified release archive were compared with that commit and have
identical Git blob IDs.

| Source | Behavior (source-backed) |
| --- | --- |
| `src/arvbuffer.c:379-398` | `arv_buffer_get_timestamp()` returns `priv->timestamp_ns` as `guint64`. The documentation calls it the "camera timestamp", but adds that "not all devices provide reliable timestamp". It names no event (exposure start or end, readout, transmission). |
| `src/arvbuffer.c:401-417` | `arv_buffer_set_timestamp()` lets any caller overwrite the value. The adapter never calls it. |
| `src/arvbuffer.c:420-437` | `arv_buffer_get_system_timestamp()` is "the system timestamp for when the frame was received". It is a reception time, not integration start. |
| `src/arvfakecamera.c:755`, `:786-787` | The Fake fill sets `timestamp_ns = g_get_real_time() * 1000`, then sets `system_timestamp_ns` to that same value. This is the host **wall clock** (`CLOCK_REALTIME`), sampled while filling the buffer. It is not steady, not camera time, and not exposure start. |
| `src/arvfakecamera.c:214-236`, `:242-247` | Fake frame pacing sleeps toward the next wall-clock multiple of the frame period. No exposure is simulated in time. The exposure value only scales the pixel ramp. |
| `src/arvfakestream.c:85-101` | The Fake stream thread waits, pops an input buffer, fills it with `arv_fake_camera_fill_buffer()` and pushes it to the output queue. No GVSP is involved. |
| `src/arvfakeinterface.c:78`, `src/arvfakedevice.c:202` | Each open of the Fake interface creates a new `ArvFakeCamera` with default registers. |
| `src/arvfakecamera.c:1074-1075` | The Fake *camera object* advertises a 1 GHz tick frequency in its GVBS registers. That value is used only by the separate `arv-fake-gv-camera` GigE simulator. The in-process `ArvFakeDevice` is not an `ArvGvDevice`, so the supported accessor below is not available for it. |
| `src/arvgvstream.c:458` | GigE: `system_timestamp_ns = g_get_real_time() * 1000LL` when the leader packet is processed. This is host wall-clock reception time. |
| `src/arvgvstream.c:466`, `src/arvgvspprivate.h:389-401` | GigE: the 64-bit raw tick value is read from the GVSP **leader** (`timestamp_high`/`timestamp_low`). |
| `src/arvgvstream.c:480-484` (also `:489-493`, `:522-528`) | GigE: if the stream's tick frequency is non-zero, the SDK converts with `arv_gvsp_timestamp_to_ns()`. **Otherwise it substitutes the host reception wall-clock time.** Unknown payload types always use the host time. |
| `src/arvgvspprivate.h:576-591` | The conversion is unchecked `guint64` arithmetic: `ts / f * 1e9 + (ts % f) * 1e9 / f`. It returns 0 when the frequency is below 1. |
| `src/arvgvstream.c:1613`, `:1640` | The tick frequency is read **once**, at stream construction, through `arv_gv_device_get_timestamp_tick_frequency()`. A read error is ignored, giving 0 and therefore the host fallback. |
| `src/arvgvdevice.c:565-591`, `src/arvgvcpprivate.h:116-117` | That accessor reads GVBS bootstrap registers `0x093C`/`0x0940`, the timestamp tick frequency high and low words. |
| `src/arvgvsp.c:68-87`, `src/arvgvfakecamera.c:352-354` | The SDK's own GigE *simulator* writes `arv_buffer_get_timestamp()` of its Fake-filled buffer into the leader. Even that simulated "camera" timestamp is therefore host wall-clock time. |

**What these facts do and do not prove.** A non-zero frequency makes a numeric
conversion possible. It does **not** prove any of the following:

- that the leader timestamp marks integration start (GigE Vision does not
  require a specific event);
- what the epoch is, whether it is UTC, or that PTP is synchronized;
- that the clock is continuous across reset, reconnection or restart;
- that the value is physically accurate.

A frequency of zero means the "camera" timestamp is really host reception
time. Provenance claims come from the source above, never from comparing two
observed numbers. Equal values on the Fake are *consistent with* lines
786-787, but they are not the evidence.

### Read-only observation path

`aravis::receive_with_native_timestamps(stream, timeout)` (in
`aravis_backend.hpp`) is **exactly** `IFrameStream::receive()`. The deadline,
validation, pool accounting, lease owner and queue/release order are all the
same. It also returns a `NativeTimestampObservation` for the buffer behind the
returned lease:

- the SDK frame ID, stream generation, and image region (width, height,
  offset X/Y);
- `arv_buffer_get_timestamp()` and `arv_buffer_get_system_timestamp()`,
  copied as unconverted `uint64_t`. No signed-nanosecond conversion is done,
  so no overflow can occur.

Design constraints:

- The observation is read on the **receiving thread**, never on the SDK
  receiver thread. It is read after the production validator accepted the
  buffer and before ownership moves to the lease.
- It uses four constant-time public getters, and only when requested.
- No callback, hook, global state or allocation is involved. The values
  travel only in the call's return value.
- It does not use `TestHooks`, so it cannot be confused with the test-only
  fault seams. The decoded-facts seam cannot alter the observation, which is
  read from the SDK buffer directly.
- `FrameDescriptor::timestamp` is assigned `FrameTimestamp{}` before
  observation and is never derived from it.
- Discovery settings, interface enable flags and SDK-global callbacks are not
  touched.

`aravis::read_timing_context(device)` reads the following, without writing
anything:

- the native device's GObject type (`ArvGvDevice`, `ArvFakeDevice`);
- the implemented identity features (`DeviceVendorName`, `DeviceModelName`,
  `DeviceSerialNumber`, `DeviceVersion`, `DeviceFirmwareVersion`,
  `DeviceManufacturerInfo`, `DeviceID`);
- the byte size and SHA-256 of the GenICam XML the SDK loaded
  (`arv_device_get_genicam_xml()`);
- **only for an `ArvGvDevice`**, the tick frequency through the supported
  accessor above.

A failed query is an error, never treated as absence. The frequency is this
diagnostic's own pre-stream read, not the value the SDK stream samples during
its construction (GIGE-MEL-010-R1; see
[the `time-probe` provenance fields](#diagnostic-tick-frequency-observation-versus-the-streams-own-read)).
No frequency is ever
derived from frame intervals.

### `time-probe` diagnostic command

```sh
build/aravis-dev/gige_mel_aravis_diagnostic time-probe DEVICE_ID COUNT OUTPUT.jsonl
```

- **Interface:** the production GigE Vision interface only, with exact device
  ID selection and no Fake fallback.
- **Validation before any device I/O:**
  - `COUNT` must be a decimal integer in **[1, 32]**;
  - `DEVICE_ID` and `OUTPUT.jsonl` must be non-empty;
  - `OUTPUT.jsonl` is created **exclusively**: an existing file is never
    overwritten, and the command fails before opening the device.
- **Bounds:** one overall receive deadline of **10 s** for all frames, which
  is also the maximum the routine accepts. Each JSONL line is at most 4096
  bytes, so the file holds at most 34 lines.
- **Read-only:**
  - it uses the current supported configuration and writes no feature;
  - it changes no trigger, PTP, network or packet setting;
  - it issues only `AcquisitionStart`/`AcquisitionStop`;
  - each lease is released immediately.

  Constructing a GigE stream still runs the SDK's own transport setup, which
  this project does not control (`src/arvgvstream.c:1616-1620`, `:1670-1673`).
  These are the stream destination address and port and, only when the
  device reports none, a default packet size. This is the same set-up
  `capture` already performs.
- **Cleanup on every path (GIGE-MEL-010-R1).** Two RAII guards own the
  resources as soon as each one exists:
  - The **output guard** owns only a pathname that this call created with
    `fopen("wx")`. It closes the descriptor exactly once. It removes the path
    on every ordinary error return and during exception unwinding, until the
    complete report has been written, flushed and closed. When exclusive
    creation fails, the guard owns nothing, so an existing file is never
    removed or changed.
  - The **native session** owns the opened device and, once created, its
    stream. It runs the established cleanup exactly once: `stop()`
    (noexcept; retires the native stream and pool), destruction of the stream
    wrapper, then `close()`. There is no second native stop and no
    `arv_shutdown()`.

  The JSONL report is still rendered and written only **after** a clean stop
  and close. Error reporting:
  - The first failure goes in `error`. An exception escaping `open()`,
    acquisition, rendering or writing is caught and reported there as
    "time probe failed: …". After an open, the message adds "the device was
    stopped and closed". In all cases it ends "the output was removed".
  - A stop or close failure is reported separately in `cleanup_error`. It
    never replaces the first failure.
  - When `open()` itself fails or throws, no device cleanup is invented.
  - An allocation failure while composing a message may leave that message
    incomplete. The guards still release the device and remove the output.
- **Exit status:** 0 on success, 1 on any operational, output or cleanup
  failure, 2 on usage errors.
- **Shared code:** the logic is `aravis::time_probe()`
  (`aravis_time_probe.cpp`). Tests call the same routine with the private
  Fake backend; production interface selection is untouched.

The output contract is schema `gige-mel.aravis-time-probe`, **version 2**
(GIGE-MEL-010-R1). Version 1 is superseded. Its `tick_frequency_hz`,
`tick_frequency_source` and GigE `sdk_buffer_timestamp_source` wording could
be read as proof of the stream's timestamp path, so the meaning changed and
the version was incremented.

| Line | Fields |
| --- | --- |
| 1 `header` | `device_id`, `interface` (`gige_vision`/`fake`/`other`), `native_device_type`, `compiled_sdk_version`, `runtime_sdk_version`, `identity` (implemented features plus GenICam XML size and SHA-256), `diagnostic_tick_frequency_hz` and `diagnostic_tick_frequency_read`, `actual_stream_timestamp_path` and `actual_stream_timestamp_path_basis`, `sdk_stream_timestamp_paths`, `sdk_buffer_timestamp_source`, `sdk_system_timestamp_source`, `timing_origin`, `configuration`, `requested_frames`, `receive_deadline_ms`, `timestamp_units`, `timestamp_qualified: false`, `qualification_reason` |
| 2 … COUNT+1 `frame` | `index`, `device_id`, `interface`, both SDK versions, `stream_generation`, `native_frame_id`, `width`, `height`, `offset_x`, `offset_y`, `sdk_buffer_timestamp_ns`, `sdk_system_timestamp_ns` (unsigned, unconverted), `actual_stream_timestamp_path`, `descriptor_timestamp_valid: false`, `timestamp_qualified: false`, `qualification_reason` |
| last `summary` | `frames`, the stream statistics, `timestamp_qualified: false` |

#### Diagnostic tick-frequency observation versus the stream's own read

The pinned SDK reads the GigE tick frequency twice, independently:

1. **This diagnostic** reads it once with
   `arv_gv_device_get_timestamp_tick_frequency()` (GVBS `0x093C`/`0x0940`)
   in `read_timing_context()`, **before** any stream exists. A read error
   makes the probe fail. It is never reported as an absent frequency.
2. **The SDK stream** reads it again during its own construction
   (`src/arvgvstream.c:1613`, `arv_gv_device_get_timestamp_tick_frequency
   (priv->gv_device, NULL)`) and stores that value for its receiver thread
   (`:1640`). Here the error is **ignored**, so a failed read silently
   becomes 0. This value is not exposed to the adapter.

Only the second value selects the GigE timestamp branch
(`src/arvgvstream.c:480-484`, also `:489-493`, `:522-528`):

- **Non-zero:** the GVSP leader ticks are converted to nanoseconds by
  `arv_gvsp_timestamp_to_ns()`.
- **Zero:** the SDK substitutes its host reception wall-clock time.

The two reads are separate register transactions at different times. They
can return different values, or one can fail while the other succeeds. For
example, the diagnostic reads 1 000 000 000 Hz, the stream's read fails, and
the stream silently uses host reception time. The reverse can also happen.
The diagnostic therefore reports:

| Field | GigE (`ArvGvDevice`) | Fake (`ArvFakeDevice`) |
| --- | --- | --- |
| `diagnostic_tick_frequency_hz` | The diagnostic's observed value, exact and unsigned (including 0) | `null`: not an `ArvGvDevice`, so no supported accessor; never guessed |
| `diagnostic_tick_frequency_read` | "separate read … before stream creation; NOT the value the SDK stream sampled during its own construction … not proof of the stream's timestamp path" | "not read: the in-process Fake device is not an ArvGvDevice" |
| `actual_stream_timestamp_path` | **`"unverified"`** in the header and every frame record | `"pinned_fake_fill_host_wall_clock"` |
| `actual_stream_timestamp_path_basis` | "not established by this diagnostic …" | Pinned source: `src/arvfakestream.c:93`, `src/arvfakecamera.c:786-787`; no GigE stream is constructed |
| `sdk_stream_timestamp_paths` | Object with `stream_construction_frequency_source`, `stream_construction_frequency_nonzero` and `stream_construction_frequency_zero`: the conditional SDK behavior above, with citations | `null` |
| `sdk_buffer_timestamp_source` | "conditional on the tick frequency the SDK stream read during its own construction … which branch this stream took is unverified" | "pinned SDK Fake fill: g_get_real_time() * 1000 (src/arvfakecamera.c:786)" |

The GigE wording is the same text whatever the observed frequency is. The
number is the only difference. No path is ever inferred from the diagnostic
sample, from equal or unequal observed timestamps, from frame intervals, or
from plausible-looking magnitudes. A successful conversion would still not
prove integration start, a clock epoch, UTC, PTP synchronization,
continuity or accuracy. The serializer refuses an inconsistent context: a
GigE context without its read result, or a frequency for a device that is
not an `ArvGvDevice`.

The Fake statement is source-established, because the in-process Fake stream
has exactly one fill path. Nothing here exercises GigE stream construction or
a physical lost-frequency event. The GigE rows are tested only with synthetic
private `TimingContext` values passed through the production serializer.

#### Exact UTF-8 strings

Every string, including identity values, the device ID and object keys, is
written as exact UTF-8 by the production encoder
`time_probe_json_string()`:

- **Well-formed UTF-8** (Unicode Table 3-7) is copied byte for byte. Only
  the characters JSON requires are escaped: `\"`, `\\`, `\b \f \n \r \t`,
  and `\u00XX` for the other U+0000-U+001F (including an embedded NUL) and
  DEL. Decoding the JSON returns the original string exactly, for example
  `Cámara – König`, `カメラ Κάμερα` or `📷`.
  Superseded: version 1 wrote each byte ≥ 0x80 as `\u00XX`, which decoded
  `Cámara – König` as the mojibake `CÃ¡mara â\u0080\u0093 KÃ¶nig`.
- **Malformed UTF-8 is an error and is never repaired.** This covers a
  stray or missing continuation byte, an overlong form (`C0`/`C1`, `E0 80`–`9F`,
  `F0 80`–`8F`), a UTF-16 surrogate (`ED A0`–`BF`), a code point above
  U+10FFFF (`F4 90`+), bytes `F5`–`FF`, and a truncated final sequence.
  - The probe fails with an `io_error` that names the field, for example
    `'header.identity.DeviceModelName'`, and gives the byte offset and bytes.
  - Native cleanup has already run, and the output file is removed.
  - No U+FFFD, byte escape or replacement identity is ever written, so a
    report cannot silently misidentify a camera.
  - A malformed `DEVICE_ID` argument is refused as `invalid_argument`
    before any output or device I/O.

The `timing_origin` field depends on the device:

- **Fake:** "pinned Aravis 0.8.36 private Fake device: both SDK timestamps are
  the host wall clock (g_get_real_time) sampled when the SDK fills the buffer;
  not camera time, not exposure start, not host-monotonic".
- **GigE:** "unverified … without a camera-specific timing profile".

Fake output (abridged, schema version 2, from an `aravis_timestamp_contract`
run; reports are not committed, and no physical camera was used):

```text
{"record":"header","schema":"gige-mel.aravis-time-probe","schema_version":2,"device_id":"Fake_1","interface":"fake","native_device_type":"ArvFakeDevice",…,"diagnostic_tick_frequency_hz":null,"diagnostic_tick_frequency_read":"not read: the in-process Fake device is not an ArvGvDevice","actual_stream_timestamp_path":"pinned_fake_fill_host_wall_clock",…,"sdk_stream_timestamp_paths":null,"sdk_buffer_timestamp_source":"pinned SDK Fake fill: g_get_real_time() * 1000 (src/arvfakecamera.c:786)",…,"timestamp_qualified":false,…}
{"record":"frame","index":0,"device_id":"Fake_1","interface":"fake",…,"stream_generation":2,"native_frame_id":65401,"width":512,"height":512,"offset_x":0,"offset_y":0,"sdk_buffer_timestamp_ns":1793507721560068000,"sdk_system_timestamp_ns":1793507721560068000,"actual_stream_timestamp_path":"pinned_fake_fill_host_wall_clock","descriptor_timestamp_valid":false,"timestamp_qualified":false,…}
```

A synthetic GigE header (abridged; production serializer, synthetic
`TimingContext` with a 1 GHz diagnostic read; no camera):

```text
{"record":"header",…,"interface":"gige_vision","native_device_type":"ArvGvDevice",…,"diagnostic_tick_frequency_hz":1000000000,"diagnostic_tick_frequency_read":"separate read by this diagnostic … before stream creation; NOT the value the SDK stream sampled during its own construction, which may differ or fail; not proof of the stream's timestamp path","actual_stream_timestamp_path":"unverified","actual_stream_timestamp_path_basis":"not established by this diagnostic: …","sdk_stream_timestamp_paths":{"stream_construction_frequency_source":"… (src/arvgvstream.c:1613, stored at :1640); a read error is ignored and yields 0","stream_construction_frequency_nonzero":"GVSP leader ticks converted to nanoseconds … (src/arvgvstream.c:480-482); …","stream_construction_frequency_zero":"the SDK substitutes its host reception wall-clock time … (src/arvgvstream.c:484); not camera time"},…,"timestamp_qualified":false,…}
```

### Test-only virtual integration-start clock

`tests/support/aravis_virtual_clock.*` is a decorator around the **real**
Aravis Fake backend. It is linked only into `aravis_timestamp_contract_tests`.
It is not part of `gige_mel_aravis`, the provider or the diagnostic, and no
environment variable or setting enables it.

What it does:

- It forwards every backend, device and stream call unchanged.
- It obtains each frame from the adapter's own `IFrameStream::receive()` and
  copies the descriptor unchanged **except the timestamp**. The timestamp is
  set from a declared **test model**:

  ```text
  domain = device, event = integration_start, valid = true,
  clock_generation = fixed per run,
  value = virtual_epoch + sequence * virtual_frame_period   (checked; overflow -> receive error)
  ```

  `sequence` counts the decorated stream's received frames since `start()`.

What it never does:

- read or write the SDK buffer timestamp;
- subtract exposure from anything;
- relabel completion time;
- modify pixels, frame IDs, geometry or generations.

**Ownership.** The decorated `FrameLease` exposes the original span and holds
the **original** lease inside a shared `SourceHold` owner. The SDK buffer is
requeued or freed exactly once, by the adapter's own `LeasedBuffer`, after the
provider drops the decorated lease. This is never while its bytes are visible
to the provider.

A pass-through mode (no clock) is the control: the same decorator, the same
genuine frames, the original timestamps.

### `aravis_timestamp_contract` (13th SDK-enabled CTest entry)

It has 17 scenarios: the 12 GIGE-MEL-010 scenarios plus five GIGE-MEL-010-R1
regressions (P1-P5).

The scenarios use the unchanged common provider through
`make_api_manager(settings, decorated Fake backend)`, the upstream
`getControl`/`getBuffer`, and registered `0xA5`-sentinel destinations. The
settings are `timestamp_domain = device` and **virtual** optics
(0.0002/0.0003 rad/px).

| Group | Evidence kind | Behavior |
| --- | --- | --- |
| O1 | Real SDK, native observation | Raw Fake values (`buffer == system`, inside the wall-clock window of the receive); descriptor timestamp stays invalid; ID/generation/ROI match; every pixel matches the oracle; four held leases starve the pool; one release requeues; the next plain receive returns the next consecutive ID; foreign stream refused; closed device refused |
| O2 | Diagnostic routine | Bad ID/COUNT (0, 33)/deadline (0, 10001 ms)/empty output, and an existing output file: rejected with **zero** device opens; existing file untouched; unknown device: `not_found`, output removed |
| O3 | Diagnostic routine, real SDK | 6-frame probe against the private Fake: no feature write, exactly Start/Stop, no live stream or buffer afterwards; 8 lines (exactly 1 header, 6 frames, 1 summary), each parsed with the vendored nlohmann/json; schema version 2; every required field present; `diagnostic_tick_frequency_hz` `null` and no schema-1 `tick_frequency_hz`; `actual_stream_timestamp_path` `pinned_fake_fill_host_wall_clock` in header and frames; consecutive IDs; values inside the wall-clock window; **no ordering of values asserted** |
| V0 | Test model | Checked arithmetic boundaries; an overflow becomes a receive error and the genuine frame is still released |
| V1 | **Virtual clock**, real MEL callbacks | 12 published frames, each checked independently in the callback. Destination == original SDK bytes (row-by-row, source lease alive) == independent pinned-Fake pixel oracle. Header: width/height, 8 bits, 1 band, Mono, Staring, ROI offsets, integration time from the readback, `systemTime` == declared virtual value, frame ID == source frame ID, virtual FOV. Registered address and context; no extra destination; statistics agree with backend deliveries; source leases peak at 1 |
| V2 | Virtual clock | All 4 destinations held: further genuine frames dropped (`dropped_no_buffer`), no callback, held bytes unchanged. Source leases still released (at most 1 outstanding). Dropping `shared_ptr` copies does not count as a release. One explicit `release()` resumes publication into exactly that slot (a second `release()` fails); other held slots keep their bytes |
| V3 | Virtual clock | `disable()` drains; Fake acquisition register 0; no late frame or callback. Reconfigure while disabled with SDK readback. Restart refused while destinations are held. After release and re-registration, a new stream generation publishes 80×40@(16,12) at 100 ms, verified against oracle scale 2.0 and a new virtual model. Old destinations unchanged; no stale frame |
| N1 | Fail-closed | Default `FrameTimestamp{}`, `valid=false`, domain `host_monotonic`/`utc`, event `reception`/`unspecified`: each rejected per frame. No callback, destination still the sentinel, no stream or callback failure, channel still enabled |
| N2 | Fail-closed | Two rejected initial frames (generation 99 and one hour ahead, or exactly `FrameTimestamp{}`), then declared frames: published, no discontinuity. A rejected frame does not set the stream's clock baseline |
| N3 | Terminal | Backward value, then (separately) a `clock_generation` change, at the 4th frame. 3 published, the 4th not written, `stream_failures == 1`, stream stopped, `enable()` reports `Fail`, no later frame. Only explicit `disable()`/`enable()` recovers, with a new generation and a fresh baseline |
| N4 | Classification | SDK-arranged `AcquisitionStart` rejection: `enable()` `Fail` and no stream failure, distinct from N3. Deliberate disable: no failure |
| C | Control | The same decorator **without** the clock: the same genuine frames are rejected; 0 callbacks |
| P1 (R1) | Production serializer, synthetic `TimingContext` | Provenance table. A1 GigE diagnostic read 1 GHz and `UINT64_MAX`, A2 GigE 0: the value is kept exactly, `actual_stream_timestamp_path` is `"unverified"` in the header and every frame, both conditional SDK behaviors are cited (`:1613`, `:480-482`, `:484`), the GigE header text is identical apart from the number, and the reviewed definite-path wording is absent. A3 GigE with no read result: refused (`invalid_state`); no line, no fabricated absence. A4 Fake: `null` frequency and the pinned fill statement. A frequency for a Fake or other device is refused. `timestamp_qualified=false` and `descriptor_timestamp_valid=false` throughout |
| P2 (R1) | Production encoder | B1 ASCII, B2 `Cámara – König`, B3 Japanese and Greek, B4 U+1F4F7 and U+10FFFF, B5 quote/backslash/controls/embedded NUL/DEL, the range boundaries and the empty string all decode with nlohmann/json to the **exact** original. No raw control byte is written. B6: 15 malformed inputs (stray, malformed 2nd/3rd/4th byte, truncated 2/3/4-byte, overlong `C0`/`C1`/`E0`/`F0`, surrogate, > U+10FFFF, `F5`, `FF`) are each refused with their reason. Through the full report: identity and device ID round-trip, and a malformed identity is refused naming `'header.identity.DeviceModelName'`. A malformed `DEVICE_ID` is refused before output or open |
| P3 (R1) | Real SDK, Fake GenICam | `DeviceModelName` (StringReg `0x68`) arranged in the Fake device memory after the native open, then read by the production `read_timing_context()`. `Cámara – König 📷`: the SDK value and the parsed JSONL string compare equal. `K\xC3(nig`: the probe fails naming the field, Start/Stop ran, no live stream or buffer, the output is removed |
| P4 (R1) | Production `time_probe()` | C1: an existing file (binary bytes, embedded NUL) is unchanged, with 0 opens. C2: argument errors give 0 opens and no file. C3: `open()` returns an Error, which is kept; the output is removed. C4: a scripted non-Aravis backend whose `open()` throws `std::runtime_error`: reported, bounded (< 2 s), output removed, no device cleanup invented; also a throw inside the real adapter's `open()` (`after_reservation`) with no native open, stream or command, and the reservation is released. C5: a deterministic throw through the test-only checkpoint seam at `opened`, `started` (stream live), `render` and `written` (header already in the file): output removed, 0 live streams or buffers, the expected Start/Stop log, and the device can be reopened |
| P5 (R1) | Real SDK, decoded-facts seam | C6: after stream creation and start, every genuine buffer is marked incomplete, so the one 300 ms deadline expires. The first failure (`timeout`, "after 0 of 2 frames") is kept. The stream and pool are retired, the device is closed and can be reopened, and the output is removed. With an SDK-rejected `AcquisitionStop` (imposed RO), `cleanup_error` reports it separately and the primary error is not replaced |

The scenario watchdog is 120 s, and an expiry exits with status 3, never a
pass. All waits are bounded at 10 s. The CTest timeout is 600 s. The binary
accepts `FILTER [REPEAT]` for focused repetitions.

The negative `aravis_mel_embedding` probe is unchanged and still passes. It
exercises the production adapter without any decorator.

### Camera-timing qualification checklist

Before an Aravis camera profile may produce a **valid** `FrameTimestamp`, every
item below must be established for the **selected physical camera**. Each
item needs retained evidence. The gate is **open**: no camera is selected and
none of these is established.

1. Camera manufacturer, model, serial, firmware and GenICam XML identity
   (`time-probe` records the XML size and SHA-256).
2. The SDK version and the actual loaded library: the path and the
   runtime/compiled version.
3. The native timestamp field (GVSP leader, chunk `ChunkTimestamp`, or event
   data) and the camera's documented acquisition event for it.
4. Proof that the event denotes **start of sensor integration**: not frame
   end, readout, trigger or transmission.
5. Tick frequency, units and checked conversion rules. The SDK conversion
   (`src/arvgvspprivate.h:576-591`) is unchecked and is not a qualification.
6. Clock origin and epoch, and whether the domain is device-local,
   host-monotonic or synchronized UTC.
7. PTP synchronization and lock-status evidence, where relevant.
8. Timestamp behavior at frame-ID wrap, stream restart, camera reset, exposure
   change and network reconnection.
9. The continuity/discontinuity policy and the rules for changing
   `clock_generation`.
10. A hardware observation or vendor-backed qualification relating the
    timestamp event to actual exposure start.
11. The maximum supported uncertainty, and its implications for the intended
    MEL consumer.
12. Behavior when any required fact becomes unavailable. For example, with a
    zero tick frequency the SDK silently substitutes host time
    (`src/arvgvstream.c:484`). This must be fail-closed (invalid timestamps),
    never a fallback.
13. Proof of the timestamp path **actually used by the qualified stream**
    (GIGE-MEL-010-R1). The SDK stream silently ignores its own
    stream-construction frequency read error (`src/arvgvstream.c:1613`). A
    separate pre-stream read, including `time-probe`'s
    `diagnostic_tick_frequency_hz`, is therefore not evidence of the path.
    A qualified profile needs a mechanism that fixes or observes the value
    the stream itself used, for example chunk timestamps with a
    camera-documented frequency, or a stream-side value it can verify. Until
    then, `actual_stream_timestamp_path` stays `"unverified"`.

None of the following is evidence:

- A monotonic series of timestamps does **not** prove integration-start
  semantics.
- A successful raw timestamp sample does **not** establish a valid clock
  domain.
- A host reception timestamp is **not** the start of exposure.
- A virtual-clock pass is **not** hardware timestamp qualification.

No generic conversion based only on `arv_buffer_get_timestamp()` and the
current exposure is implemented, and none may be.

## Loopback GigE Vision transport (GIGE-MEL-011)

### Simulator and fixture

The server is the pinned SDK's network camera simulator, `ArvGvFakeCamera`
(`src/arvgvfakecamera.c`, public API `arv_gv_fake_camera_new()`,
`arv_gv_fake_camera_is_running()` and `arv_gv_fake_camera_get_fake_camera()`).
It runs in the test process on its own thread. Its GVCP and GVSP sockets are
bound to `127.0.0.1`, but it **also** binds a wildcard global-discovery
listener on 255.255.255.255:3956 (see
[Network exposure](#network-exposure-of-the-simulator-gige-mel-011-r1)). This is
the same object the upstream `tests/fakegv.c` uses, created the same way:
`arv_gv_fake_camera_new("127.0.0.1", "GVTest")`. It answers GVCP on UDP
127.0.0.1:3956 and sends GVSP datagrams to whatever stream destination the
client programs. The upstream `arv-fake-gv-camera` program is neither built nor
used, and the SDK, its provisioning and its version are unchanged. The
provisioned build has packet sockets disabled, so the client receives GVSP on
an ordinary UDP socket. No root, `CAP_NET_RAW`, VM or dedicated NIC is needed.

The **client** is the unchanged production `aravis::make_backend()`. It is not
`ArvFakeInterface` and not the private Fake backend. A few scenarios need to
observe native objects such as the stream type, pool size or finalization
order. Those use `make_backend_for_interface()` with the **identical** interface
source (`{"GigEVision", arv_gv_interface_get_instance}`) and observation-only
hooks.

`tests/support/aravis_gv_loopback.*` (test-only, uninstalled) does the
following:

- It finds the up IPv4 loopback interface carrying 127.0.0.1 (`lo`).
- It requires UDP 127.0.0.1:3956 to be **free** before start. A busy port is an
  immediate failure with the `bind` error, never a skip.
- It restricts the **client's** GigE discovery to that interface for the test
  process through the public `arv_gv_interface_set_discovery_interface_name()`
  (0.8.34+). The client's discovery sockets are then bound to 127.0.0.1, its
  limited-broadcast discovery is routed over `lo`, and no physical camera can
  be discovered. The previous value is restored afterwards. The production
  adapter's discovery policy is unchanged. *This setting does not restrict the
  simulator's own listening sockets.* (GIGE-MEL-011-R1 supersedes the earlier
  wording "discovery broadcasts never leave loopback", which described only
  the client's outgoing discovery.)
- It checks the following after start:
  - `is_running`;
  - an `ArvFakeCamera` underneath;
  - the GVCP port now held by the simulator;
  - the bootstrap identity in server memory (IP 127.0.0.1, vendor `Aravis`,
    model `Fake`, serial `GVTest`);
  - no controller and no acquisition yet.
- `finish()` checks the shutdown order. It requires that the client has
  released GVCP control and acquisition. It then drops the simulator, whose
  finalizer joins its thread and closes its sockets. It checks finalization
  through a GObject weak reference, proves UDP 3956 is free again, and
  restores the discovery interface. The destructor is a non-throwing backstop
  for failure paths.
- Discovery waits are bounded at 10 s. Each attempt is a 1 s SDK poll. Expiry
  reports the interface, what was found, `is_running` and the port state.
- A structured UDP socket audit (GIGE-MEL-011-R1) classifies every UDP socket
  the process owns. It runs before and after simulator start, during
  acquisition, after client close, after simulator destruction and after every
  scenario. See
  [Socket audit](#socket-audit-contract-gige-mel-011-r1). (This replaces the
  earlier `require_loopback_only_sockets()`. That check accepted any socket on
  127.0.0.1 **or** 255.255.255.255, and the earlier record described the result
  as loopback-only. Both are superseded.)

`arv_shutdown()` is never called. The test runs under CTest
`RESOURCE_LOCK gige_gvcp_loopback_3956`, because port 3956 is fixed.

### Network exposure of the simulator (GIGE-MEL-011-R1)

> **Security qualification.** The GIGE-MEL-011 client restricts GigE
> discovery and normal GVCP/GVSP traffic to loopback. However, pinned Aravis
> 0.8.36's network simulator also creates a global GVCP discovery listener
> bound to 255.255.255.255:3956. On Linux that binding is effectively
> wildcard, so the simulator process is not strictly confined to loopback.
>
> `aravis_gige_loopback_contract` therefore runs under an **explicitly
> documented test-only exception**. It does not establish strict network
> isolation. Acceptance of this exception is the repository owner's decision,
> and that decision is **pending**.

**Sockets the pinned simulator creates.** Pinned commit
`97ff5e330919d7a44b871df4eda669feeafdf342`. Its `src/arvgvfakecamera.c` is
byte-identical (SHA-256 `2a7366a7…8de37`) to the file in the provisioned,
checksum-verified tarball. `arv_gv_fake_camera_start()` calls
`_create_and_bind_input_socket()` (lines 443-484). That helper uses
`g_socket_bind()`, which sets `SO_REUSEADDR` and, for datagram sockets,
`SO_REUSEPORT` when `allow_reuse` is TRUE. It never sets `SO_BINDTODEVICE`.

| | Socket (pinned lines) | Bound to | Options (observed) | Purpose |
| --- | --- | --- | --- | --- |
| A | GVCP input (518-520) | 127.0.0.1:3956 | no reuse | Control: discovery replies, register/memory access |
| B | GVSP output (516-517) | 127.0.0.1:*kernel-assigned* (port 0) | no reuse | Source of GVSP datagrams sent to the client's programmed destination |
| C | Global discovery (522-526) | **255.255.255.255:3956** | `SO_REUSEADDR=1`, `SO_REUSEPORT=1`, no device | Receives GigE Vision discovery sent to the limited broadcast address. Created whenever the GVCP address is not 255.255.255.255 |
| D | Subnet discovery (530-536) | the interface broadcast address, port 3956 | n/a | **Not created for `lo`.** It is created only if the interface broadcast address differs from the GVCP address. On Linux, the pinned enumeration copies `ifa_ifu.ifu_broadaddr` (`src/arvnetwork.c:380-381`). For `lo` that is 127.0.0.1 (`IFF_BROADCAST` clear), which equals the GVCP address. The fixture asserts this before relying on it |

**Why upstream creates C.** A GigE Vision camera must answer discovery
commands sent to the limited broadcast address 255.255.255.255:3956. The pinned
client's discovery also sends there first, and uses a directed broadcast only
as a fallback (`src/arvgvinterface.c:180-206`). On Linux, a socket bound to
127.0.0.1 does not receive datagrams addressed to 255.255.255.255. The
simulator therefore binds a separate socket to that address. It does so with
`allow_reuse` TRUE, so other reuse-enabled sockets may share the port. The
pinned source has no property or option to suppress the socket; the only
condition is that the GVCP address is not itself 255.255.255.255.

**Linux binding semantics.** ip(7): "INADDR_BROADCAST (255.255.255.255) has
the same effect on bind(2) as INADDR_ANY for historical reasons." The
kernel's `inet_bind()` keeps 255.255.255.255 as the receive address
(`net/ipv4/af_inet.c`, v6.12). So `/proc/net/udp` reports `FFFFFFFF:0F74` and
`getsockname()` returns 255.255.255.255, **not** 0.0.0.0. The following were
observed locally (Linux 6.12, no reuse options, out-of-repository probes):

- 255.255.255.255:P conflicts with both 0.0.0.0:P and another
  255.255.255.255:P;
- it coexists with 127.0.0.1:P;
- a 255.255.255.255-bound socket has no `SO_BINDTODEVICE`;
- it received a limited broadcast sent from 127.0.0.1 over `lo`.

For broadcast and multicast delivery, the kernel's `__udp_is_mcast_sock()`
compares the bound address with the packet destination and applies a device
check only if `SO_BINDTODEVICE` is set. Nothing ties C to `lo`.

**Three different things.**

1. *Intended traffic is loopback-only.* The client's discovery sockets are
   bound to 127.0.0.1. Its limited broadcast is routed via `lo`
   (`ip route get 255.255.255.255 from 127.0.0.1` gives `dev lo`). GVCP goes to
   127.0.0.1:3956, and GVSP goes from 127.0.0.1 to the client's 127.0.0.1
   port. The client's discovery-interface setting governs only the client.
2. *The binding scope is wildcard.* Socket C is bound with
   INADDR_ANY-equivalent semantics on every interface of the network
   namespace, for the simulator's whole lifetime.
3. *Unsolicited non-loopback traffic could be received.* A GVCP datagram
   that reaches this host on any interface, addressed to 255.255.255.255:3956,
   can be delivered to C. The simulator handles every input socket alike
   (`_handle_control_packet`). It answers DISCOVERY, READREG and READMEM, and
   while no controller holds CCP it also accepts WRITEREG/WRITEMEM.
   - Its reply is sent **from C to the requester's address**, so it may leave
     the host on a non-loopback interface.
   - A local GVCP READREG sent to 255.255.255.255:3956 *from 127.0.0.1* was
     answered (out-of-repository probe), which demonstrates that C is a live
     control endpoint.
   - A unicast datagram to the host's own non-loopback address on port 3956
     was **not** answered, because C only matches destination
     255.255.255.255.
   - No datagram was sent to or from an external interface or another host.
     Whether a broadcast from another host on the attached segment reaches C
     was **not** tested.

**No host firewall is relied on or claimed.** The local rule set could not be
read without root, and nothing here depends on it. No firewall rule,
`SO_BINDTODEVICE`, network namespace, VM or other restriction has been added
or validated. **GitHub-hosted runners are not claimed to isolate this
listener.** Their network isolation has not been verified.

**Lifetime.** C exists from `arv_gv_fake_camera_new()` (inside
`lb::Simulator`'s constructor) until the simulator is finalized in
`Simulator::finish()` (or the non-throwing destructor on a failure path).
The pinned `arv_gv_fake_camera_stop()` (lines 571-591) joins the simulator
thread and then closes A, C (`input_sockets[]`) and B.

- Each scenario and the audit case creates its own simulator, so C exists
  only while one of them runs.
- On a clean run that is about 45 s in total, across 12 sequential simulator
  instances in 11 cases (L uses three).
- A watchdog abort (`_Exit(3)`) or a crash closes it with the process.

**Cleanup verification.** `finish()` requires the following after
destruction:

- the GObject weak reference fired;
- an audit with **no** permitted exception finds no wildcard or non-loopback
  UDP socket, and none of the three recorded inodes;
- `bind(127.0.0.1:3956)` succeeds again;
- `bind(255.255.255.255:3956)` without reuse succeeds. This is
  namespace-wide: no process holds 255.255.255.255:3956 or 0.0.0.0:3956;
- the previous discovery-interface name is restored and read back.

The test driver repeats the audit with no exception and both bind probes
after every case. These checks show the release. They do **not** show that
the simulator was unreachable through another interface while it was
running.

**Recommended environment.** Run `aravis_gige_loopback_contract` only where
the operator accepts this temporary discovery-listener exposure, or inside an
independently established isolated network environment, such as a network
namespace without external interfaces or an isolated VM/container network.
That environment must be set up and verified outside this repository. Do not
run it on a host attached to a production camera network or to untrusted
networks without that acceptance. The other SDK tests use the in-process Fake
interface and open no sockets of this kind.

### Socket audit contract (GIGE-MEL-011-R1)

`tests/support/aravis_gv_loopback.*` (test-only, uninstalled) provides the
audit.

**Socket inventory.** `owned_udp_sockets()` joins `/proc/self/fd` (socket
inode → fd) with `/proc/net/udp` **and** `/proc/net/udp6` by inode, so every
listed socket belongs to this process. It reads `SO_TYPE`, `SO_PROTOCOL`,
`SO_REUSEADDR`, `SO_REUSEPORT` and `SO_BINDTODEVICE` from the same fd. The fd
must keep the same inode before and after the reads.

**Classification.** `classify_udp_socket(socket, listener)` is pure and
returns one of three values:

- `loopback`: IPv4, bound to 127.0.0.1.
- `simulator_global_discovery`: only when all of the following hold:
  - IPv4 UDP datagram socket;
  - bound to exactly 255.255.255.255, port exactly 3956;
  - `SO_REUSEADDR` and `SO_REUSEPORT` set, and no bound device;
  - **the same inode and fd as the listener the live simulator recorded at
    start**.

  With no live simulator (`listener == nullptr`), nothing qualifies.
- `unexpected`: everything else, including every other 0.0.0.0,
  255.255.255.255 or IPv6 socket.

**Audit result.** `audit_udp_sockets()` and `require_socket_audit()` return a
`SocketAudit`. It holds:

- the loopback-bound sockets;
- the matched listener(s);
- the unexpected sockets, with full address, port, inode, fd and options;
- the expected listener count: 1 with a live simulator, else 0;
- whether the exception was observed.

`summary()` names the upstream source. `passed()` requires zero unexpected
sockets and exactly the expected listener count.

**Identification at start.** First, the fixture requires that nothing
conflicts with 255.255.255.255:3956 or 0.0.0.0:3956, and that the process
owns no wildcard UDP socket. It then identifies the sockets that appeared
across construction. These must be exactly A, B and C above, with the options
shown, and D must be absent; anything else fails. C's identity is recorded
for the classifier.

| Audit point | Exception permitted | Checked |
| --- | --- | --- |
| Before simulator start | none | no wildcard socket |
| After simulator start (A, S) | the recorded listener | 2 loopback + 1 listener, 0 unexpected |
| During GVCP/GVSP acquisition (C) | the recorded listener | the client's GVSP socket is loopback-bound; 0 unexpected |
| After client close (A) | the recorded listener | only the simulator's 2 loopback + 1 listener remain |
| After simulator destruction (every `finish()`) | none | no wildcard socket; recorded inodes gone; both bind probes succeed |
| After every case (driver) | none | the same, plus both bind probes again |

**Audit-control case S.** It runs in the same executable and the same CTest
entry. It is not a transport scenario.

- The listener classifies as `simulator_global_discovery` only with its
  recorded identity, and as `unexpected` with no listener. It is never
  counted as loopback.
- Forged variants are all `unexpected`: 0.0.0.0, port 3957, a different
  inode, no `SO_REUSEPORT`, bound to `lo`, non-UDP and IPv6.
- Three real temporary sockets of this process are bound one at a time:
  - `0.0.0.0` on a kernel-assigned ephemeral port;
  - `255.255.255.255` on an ephemeral port;
  - a lookalike `255.255.255.255:3956` with the same reuse options.

  For each one, the audit must report exactly that socket as the one
  unexpected socket and still recognize the simulator's listener, and the
  checked form must throw. The socket is then closed and the normal audit
  must pass again.

S sends no datagram and changes no host setting.

### Identity, SDK and provenance

| Item | Observed |
| --- | --- |
| Discovery interface | `lo` (127.0.0.1) |
| Discovered device ID (exact, from discovery) | `Aravis-Fake-GVTest` |
| Display string | `Aravis Fake serial GVTest (GigEVision 127.0.0.1)` |
| Device address | 127.0.0.1:3956 (`arv_gv_device_get_device_address`) |
| Native device / stream types | `ArvGvDevice` / `ArvGvStream` |
| SDK | 0.8.36 runtime and compiled; loaded from the provisioned `libaravis-0.8.so.0` (`dladdr`, printed by the test) |
| Adapter | `gige_mel_aravis` is a static archive linked into the test executable (also printed). `ldd` cannot show the adapter revision; build/link provenance is the commit and build directory recorded in [validation](validation.md) |

### Scenarios (`aravis_gige_loopback_contract`, 14th SDK-enabled CTest entry)

| Group | Behavior |
| --- | --- |
| A | Discovery through `make_backend()->enumerate()` returns exactly one device: the simulator. The opened device is `ArvGvDevice`, not `ArvFakeDevice`, and is GVCP controller (the server's CCP register is nonzero). Identity features are read over GVCP. A nonce written into **server** memory is read back by the client twice, so the reads are not cached. A competing open from the same or another backend is `busy`. SDK aliases (`Aravis-GVTest`, `GVTest`, `127.0.0.1`) and an absent ID are `not_found`. Close releases control on the server, and a reopen succeeds |
| B | The initial configuration is read over GVCP (the pinned defaults 512×512@(0,0), 25 Hz, 10 ms). Capabilities are the actual bounds (sensor 2048×2048, increments 1). 64×48@(8,4), 25 Hz, 10 ms is applied through the existing transaction and proved by client readback **and** the simulator's registers: width/height/offsets, `ExposureTimeAbs` 10 000 µs, frame period 40 000 µs, `GainRaw` 0, `PixelFormat` Mono8, `TriggerMode` Off, Continuous, free-running. There are no hidden trigger or format changes. An out-of-range request is refused before any write. While acquiring, `apply()` is `invalid_state` and the server registers are unchanged. After stop, 80×40@(16,12), 10 Hz, 100 ms is applied and delivered (oracle scale 2.0) |
| C | `create_stream()` does no native work. `start()` creates one `ArvGvStream` and four pool buffers, and programs the server's SCDA0/SCP0 to 127.0.0.1 and a UDP port **owned by this process**. Eight frames are received: complete Mono8, 64×48, stride 64, 3 072 bytes, configuration and stream generations, native frame IDs (the simulator's 16-bit counter starts at 65 401), and **every pixel** equal to `(x + y + id) % 255`. The SDK's own counters show the frames were reassembled from GVSP datagrams: at packet size 1 400, 40 packets for 8 frames (leader, 3 payload, trailer), 8 completed buffers, 0 failures, 0 missing packets. Stop closes the stream channel (SCP0 0) and the UDP socket |
| D1 | `AcquisitionStart`/`Stop` are observed on the **server's** acquisition register, independently of frame arrival. A held lease's bytes are unchanged after stop while the simulator keeps running. The restart lease rule holds: restart is refused while a lease is held. The native stream is finalized before the native device (weak-reference ordinals), and the device before the reservation is released, after which another backend reopens. A reopen while an old lease is held, and that lease's release on another thread, free its buffer and never requeue it into the new `ArvGvStream` (the pool stays at 4). An explicit stop/restart produces a new stream generation |
| D2 | All four pool buffers are leased, and receive is observed inside native timed pops while GVSP keeps arriving. `request_stop`, `stop`, an exception unwind and `close` each return within 1 s (observed: 4 ms), with `stopped`/`disconnected` and no timeout. The server shows the acquisition stopped. Held leases are still valid. A fresh open acquires again |
| E1 | The production `capture_one()` over GigE. An independent check confirms the exact `P5\n64 48\n255\n` header, 3 085 bytes, every pixel against the oracle for the reported native frame ID, and stream/device retirement. The device can be reopened |
| E2 | The production `time_probe()` over GigE. An existing output file is refused with no device I/O (no controller seen by the server). Six frames produce exactly 1 header, 6 frames and 1 summary, parsed with the vendored nlohmann/json: schema version 2, `interface` `gige_vision`, `native_device_type` `ArvGvDevice`, SDK identity, and `GenICamXmlBytes`/`GenICamXmlSha256` equal to the XML the **simulator** serves. `diagnostic_tick_frequency_hz` is 1 000 000 000 (equal to the server's bootstrap registers) with its stated source. Raw unsigned SDK buffer/system timestamps equal the routine's report. Frame IDs and generations are correct. `timestamp_qualified=false` everywhere, `descriptor_timestamp_valid=false`, and `actual_stream_timestamp_path` is `"unverified"` |
| M1 | The **test-only** virtual-clock decorator wraps `make_backend()` and feeds the unchanged provider (`make_api_manager`, `Control`, `ImageChannel`, registered sentinel buffers, `onImage`). In each callback, every destination byte equals the original SDK lease bytes **and** the oracle. The header has geometry, ROI, integration time 10 ms, source native frame ID, and `systemTime` equal to the declared virtual value. Address and context are registered. With all four destinations held, further network frames are dropped and held bytes are never overwritten. An explicit `Buffer::release()` resumes into exactly that slot. `disable()` stops acquisition on the server, and there is no stale publication |
| M2 | The same decorator as a pure pass-through. Genuine GVSP frames reach the provider with `FrameTimestamp{}` and are rejected (`rejected_frames` ≥ 6, "timestamp contract"). There are no callbacks or stream failures, and the destinations keep their sentinel bytes |
| L | **Simulated** loss through the pinned construct property `gvsp-lost-ratio`, set on a fresh simulator before its thread exists. At 0.0, frames are delivered normally. At 1.0 the simulator drops every outgoing GVSP packet before `sendto`: 0 packets and 0 frames reach the client, three `receive(400 ms)` calls each return `timeout` within the bound, the pool stays at 4 buffers, a blocked receive is cancelled promptly, and stop/close release everything. Through the MEL provider, even with the virtual clock, nothing is published or rejected and destinations are unchanged |

No exact count of lost packets or resend events is asserted. The pinned GigE
device disables resend because the simulator does not advertise it. A timeout
under simulated loss says nothing about physical disconnection, and the test
asserts nothing of the kind. This is not a switch or NIC loss test.

The same executable also runs the R1 audit-control case **S**
([Socket audit](#socket-audit-contract-gige-mel-011-r1)). S is not a
transport scenario. The ten transport groups above are unchanged.

### What loopback does and does not show

The simulator's GVSP leader timestamp comes from the Fake's host wall clock
(`arv_fake_camera_fill_buffer`, pinned `src/arvfakecamera.c:786`). It is **not**
integration start, and the diagnostic still reports the GigE stream path as
`"unverified"`. Loopback has no physical NIC, MTU or switch, no real packet
loss or reordering, no real heartbeat timeout and no camera firmware, so it
says nothing about throughput or timing. The pixels are synthetic. A positive
virtual-clock callback proves the network-image publication path, not camera
timing. Production Aravis-backed MEL Image publication is still not enabled.

"Loopback" describes the **intended traffic**: the client's discovery and all
GVCP/GVSP exchanges. It is **not** a claim that the test process is confined
to loopback. The simulator's 255.255.255.255:3956 discovery listener is
wildcard-bound for the simulator's lifetime (see
[Network exposure](#network-exposure-of-the-simulator-gige-mel-011-r1)).

## Qualification boundaries

The evidence covers the project adapter and tests, executed against the real
Aravis 0.8.36 library, its in-process Fake camera and (GIGE-MEL-011) its
loopback network simulator `ArvGvFakeCamera`. It does **not** establish:

- generic GenICam or GigE Vision compatibility;
- behavior on any physical camera or firmware;
- GVCP/GVSP behavior on a physical NIC or switch, real packet loss, jumbo
  frames, multiple interfaces, firewalls or real heartbeat timeouts. Loopback
  exercises the SDK's own simulator only; its controller privilege and
  heartbeat are the simulator's implementation;
- thread safety of Aravis itself;
- camera timing, timestamps, PTP/UTC, disconnection recovery or hardware
  performance. Source acquisition is exercised with the SDK's in-process Fake
  stream and with GVSP from the loopback simulator, not with a camera;
- any Aravis integration-start timestamp. GIGE-MEL-010 observes raw SDK
  values. Its positive MEL publication uses a **test-only virtual clock** and
  is not hardware timestamp qualification (see
  [Native timestamp provenance](#native-timestamp-provenance-gige-mel-010));
- network isolation of the loopback test. The simulator's wildcard
  255.255.255.255:3956 discovery listener is a documented test-only
  exception. No `SO_BINDTODEVICE`, namespace, firewall or runner-isolation
  property is implemented or verified, and owner acceptance of the exception
  is pending.

The first physical-camera qualification and a second real SDK implementation
remain pending. Sanitizer jobs instrument the project's code only, not the
SDK.
