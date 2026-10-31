# Bootstrap validation record

## GIGE-MEL-008-R1 review corrections (2026-10-31)

This is an ordinary corrective commit on
`feature/gige-mel-008-aravis-control` (PR #8). The starting revision was the
reviewed head, which had not advanced: commit
`315a0ed559b6b245e1ddc2b7697f65a2a0439d22`, tree
`5687099e0524e1abfe8714f4a8ff2f69f9cb43fc`. Main was unchanged at
`039de755644ad2b15dd5827878ae71c028218e8f` (tree
`01476757e59fec54f66de1f3fa0820f0429b7375`). The environment and the
provisioned Aravis 0.8.36 prefix are the same as in the GIGE-MEL-008 record
below. The SDK, GLib and libxml2 remain **uninstrumented**; the sanitizers
cover only the project's adapter, tests and provider.

### Corrections

- **A. Native facts are validated before arithmetic.** `check_limits` and
  `check_current` run on every snapshot inside `capabilities_from` and
  `plan_writes`; no caller is trusted. They cover:
  - sensor extent;
  - ROI increments, minimum ≤ maximum, and supported grid origins;
  - scalar bounds and increments;
  - current ROI geometry.

  ROI comparisons are written so that they cannot overflow
  (`offset ≤ sensor − size` instead of `offset + size ≤ sensor`). A zero
  requested size is rejected even if a native minimum would permit it.
  Structural safety is kept separate from truthful readback: `configuration_from`
  is **not** required before `apply()`, and current values need not lie inside
  newly reported bounds.
- **B. Fresh constraints in prevalidation.** `plan_writes` takes every limit
  from the fresh snapshot. The open-time envelope, which backs the immutable
  published capabilities, is used only to detect a changed sensor extent, ROI
  increment or selected exposure feature. Such a change is rejected as
  `unsupported` ("reopen") before any write. The following are final before
  writes:
  - the fresh rate bounds;
  - the exposure minimum and float increments;
  - the exposure maximum, except when the period lengthens first.

  Coupled ROI maxima stay with the SDK range check, which remains enabled.
  See `docs/aravis-integration.md` for the full policy.
- **C. Optional-feature query errors.** `present()` returns `Result<bool>`:
  - absent or successfully unimplemented → `false`;
  - implemented → `true`;
  - `GError` → a translated error with feature and operation context.

  All callers were audited:
  - optional strings;
  - the optional Boolean `AcquisitionFrameRateEnable`, now read through its
    feature node;
  - exposure selection, which no longer falls back after a failed query;
  - `inspect` identity strings, which report a failure as a problem.

  A node of the wrong type is an explicit diagnostic.
- **D. Package isolation is enforced.** The reviewed step used negated
  pipelines, which bash exempts from `errexit`. **The earlier green isolation
  steps, including the GIGE-MEL-008 hosted runs, therefore did not enforce the
  negative checks and are not isolation evidence.** The step now runs
  `scripts/verify_package_isolation.py`. Violations and check-execution
  failures are both fatal, and the consumer build cannot mask them. The
  filename scan examines only descendants of the prefix.
- **E1. Concurrent open.** The reviewed test released the first opener right
  after submitting the competitor, so it never demonstrated contention. The
  test now waits, with a bound, for a private hook to report that the
  competitor's real `try_lock` of the interface lock **failed** while the
  first opener was still gated inside `open()`. Only then does it release the
  first opener. `open()` now calls `try_lock` and then `lock()` on the same
  mutex; the hook is unset in production.
- **E2. Tolerance wording.** Comments and docs now state the formula
  `|v − n| ≤ 4·DBL_EPSILON·max(1, |n|)` and call it a relative epsilon
  tolerance. The numerical policy is unchanged. The "4-ULP" wording in the
  historical record below is left as written.

### Before/after controls (A, B, C)

All controls used the same provisioned SDK,
`/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`, confirmed both by `ldd` and
by the path the test binary prints. The setup was:

- The **reviewed production code** was extracted with `git archive 315a0ed`
  into `/tmp/gm8/r1/ctl-abc`.
- Every production file (`src/backends/aravis/*`, `cmake/GigeMelAravis.cmake`,
  `CMakeLists.txt`) was compared byte for byte with 315a0ed and was identical.
- The build used `GIGE_MEL_SANITIZERS=address,undefined`. The adapter is
  linked from that build's own static `libgige_mel_aravis.a`.
- Only `tests/aravis_adapter_tests.cpp` was replaced, by the corrected tests.
  Its concurrency scenario was compiled out, because the reviewed `TestHooks`
  has no lock hook; E1 has its own control below.

Run with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`:

| Finding | Scenario | Reviewed production | Corrected |
| --- | --- | --- | --- |
| A | INT64_MIN `Width` minimum (snapshot) | UBSan halt: `camera_profile.cpp:70:42: signed integer overflow: 640 - -9223372036854775808` | `unsupported`: "Width advertises minimum -9223372036854775808 …" |
| A | INT64_MAX current `Width`, offset 16 (separate process) | UBSan halt: `camera_profile.cpp:245:26: signed integer overflow: 16 + 9223372036854775807` | `unsupported`: "Width reads back 9223372036854775807 …" |
| A | INT64_MIN `Width` minimum imposed through the SDK | attempted OffsetX, Width, OffsetY, Height, ExposureTimeAbs, AcquisitionFrameRate | rejected, zero attempts, configuration unchanged |
| B | Fresh exposure max 500 µs, current 400 µs, rate 25 Hz, request 1 ms + new ROI | attempted OffsetX, Width, OffsetY, Height, ExposureTimeAbs before the SDK rejected the exposure | `invalid_argument` "ExposureTimeAbs 1000 us is outside the native bounds [10, 500]", zero attempts |
| B | Fresh `Width` grid 8 + k·8 (current 512), opened as 1 + k·1, request 641 | accepted on the old grid; attempted Width, ExposureTimeAbs, AcquisitionFrameRate | `unsupported` "Width increment changed from 1 at open to 8 … reopen", zero attempts |
| B | Late-failure retry after the reduced maximum is visible | attempted ExposureTimeAbs again | zero attempts (collector cleared before the retry) |
| C | TriggerMode = On with a failing implementation query (SDK GError "Port not found") | open **succeeded**: the error became absence and the trigger check was skipped | open fails: "query implementation of optional feature 'TriggerMode': [TriggerMode] [GigeMelTestBrokenImplementedForTriggerMode] Port not found for node …"; no writes; native object retired; a later clean open succeeds |
| C | Failing `AcquisitionFrameRateEnable` query on an open device | `configuration()` succeeded | explicit error |
| C | Failing `ExposureTime` query | silently selected `ExposureTimeAbs` | explicit error, no fallback |

The two UBSan cases ran in a separate harness,
`/tmp/gm8/r1/ubsan_controls.cpp`, with one process per case. It was linked
against `camera_profile.cpp` compiled from either revision:

- reviewed: SHA-256 prefix `bb2d9312c4dc7443`, equal to the 315a0ed blob;
- corrected: prefix `e2d19fb020e380c5`.

The expected-failing controls are kept out of the passing suite.

### Before/after controls (E1, D)

**E1.** The reviewed production code was built with **one** additive
private-header line: a `TestHooks::interface_lock_contended` declaration that
the reviewed `open_native` never calls. `aravis_backend.cpp` was
byte-identical to 315a0ed. Against that build, the corrected concurrency test
failed at its bounded 5 s wait for observed contention ("FAIL concurrent open
across adapters: … wait_for(…, 5s, …)"), so it cannot pass without evidence at
the locking boundary. With the correction it passed 10/10 bounded repeats in
the development build, plus the sanitizer runs.

**D.** These controls exercised the committed verifier and the exact workflow
step bodies extracted from the reviewed (315a0ed) and corrected `ci.yml`. They
ran under `bash --noprofile --norc -eo pipefail`, as GitHub Actions does.
Violations were arranged with PATH stubs (`ldd`, a `cmake --install` wrapper)
or in temporary copies of the real install. They are **controls**, not
findings about the real package. The only change to the step text was mapping
the SDK-location string to the local prefix.

| Case | Reviewed step | Corrected step / verifier |
| --- | --- | --- |
| Clean install (prefix `…/gige-mel-aravis-install`) | exit 0 | exit 0 |
| Forbidden provider dependency (`libglib-2.0.so.0` added to the provider's `ldd`) | violation printed, **exit 0** | exit 1: `provider-deps … depends on forbidden SDK libraries` |
| Installed text reference (`#include <arv.h> // aravis` in `backend.hpp`) | violation printed, **exit 0** | exit 1: `installed-text include/gige_mel/backend.hpp:226 references 'arv.h'` |
| Installed child filename (`include/gige_mel/aravis_backend.hpp`) | violation printed, **exit 0** | exit 1: `installed-names … mentions Aravis` |
| Package metadata `find_dependency(Aravis)` (verifier) | — | exit 1: `installed-text … GigeMelProviderConfig.cmake:32` |
| Child filename under a prefix inside an `aravis-root/` directory | — | exit 1, for the child only |
| `ldd` fails (stub exits 1) | — | exit 1: both `ldd` checks report the command failure |
| Unreadable installed header (`chmod 000`) | — | exit 1: `cannot read include/gige_mel/provider.hpp` |
| Untraversable installed directory | — | exit 1: `cannot traverse …/include/mel` |

In every corrected failing case the consumer step still ran, and it still
succeeded wherever the package allowed it; the overall exit status stayed
nonzero.

### Local results after correction

These results come from the final corrective tree, run in the existing preset
build trees with the same provisioned SDK:

| Configuration | CTest | Provider | Backend | Portability | Aravis adapter | MEL probe | Diagnostic `--help` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GCC dev, SDK-free | **7/7** | 74/74 | 8/8 | 12 + 2 | — | — | — |
| GCC Release, SDK-free | **7/7** | pass | pass | pass | — | — | — |
| ASan/UBSan, SDK-free | **7/7** | pass | pass | pass | — | — | — |
| TSan, SDK-free | **7/7** | pass | pass | pass | — | — | — |
| Clang 19 dev, SDK-free (`clang++-19`) | **7/7** | pass | pass | pass | — | — | — |
| GCC dev + Aravis | **10/10** | 74/74 | 8/8 | 12 + 2 | **18/18** | pass | pass |
| GCC Release + Aravis | **10/10** | pass | pass | pass | 18/18 | pass | pass |
| ASan/UBSan + Aravis (`detect_leaks=1:halt_on_error=1`) | **10/10** | pass | pass | pass | 18/18 | pass | pass |
| TSan + Aravis (`halt_on_error=1`) | **10/10** | pass | pass | pass | 18/18 | pass | pass |
| Clang 19 dev + Aravis (`clang++-19`) | **10/10** | pass | pass | pass | 18/18 | pass | pass |

The adapter suite grows from 10 to **18** scenarios. All 10 earlier scenarios
are retained; the changed ones are the profile fixtures, the concurrency test
and the late-failure retry. The 8 new scenarios are:

- native fact validation;
- fresh profile constraints;
- the fresh SDK exposure limit;
- the fresh SDK `Width` grid;
- unsafe SDK ROI facts;
- optional string, optional Boolean, and exposure-selection query errors.

`ldd` shows `libasan.so.8` + `libubsan.so.1`, and `libtsan.so.2`, in the
respective adapter binaries, plus the provisioned `libaravis-0.8.so.0`. No
sanitizer report occurred. The changed concurrency scenario passed 10 bounded
repeats in the development build and 5 each under ASan and TSan.

Other checks:

- **Offline verifiers:** both pass (97 MEL files; 2 JSON files).
- **Synthetic capture:** a 64×48 PGM was captured.
- **Diagnostic:** `gige_mel_aravis_diagnostic --help` exits 0.
- **Whitespace:** `git diff --check` passes on the corrective range and on
  the full PR range from main.
- **Real package isolation:** `scripts/verify_package_isolation.py` passes on
  the real installs from the dev, Release and Clang SDK-enabled builds, each
  under a prefix named `gige-mel-aravis-install`. This covers all checks:
  adapter SDK provenance, provider dependencies, 157 installed paths,
  97 non-documentation text files, 2 installed binaries, and a consumer
  configured without `PKG_CONFIG_PATH`. These are the configurations CI
  checks.
- **Sanitizer-build installs:** the five package checks also pass, but the
  verifier correctly fails the consumer step. An uninstrumented consumer
  cannot load an ASan/TSan-instrumented provider ("ASan runtime does not come
  first" / TSan deadly signal). This is expected, and is why CI runs the step
  only in the non-sanitizer jobs.
- **Install file sets:** the default install and the separately configured
  consumer pass. The SDK-free and SDK-enabled installs have identical
  non-documentation file sets.
- **SDK absence:** with `PATH=/usr/bin:/bin` and an empty `PKG_CONFIG_PATH`,
  the default configuration succeeds with no SDK lookups in its cache.
  `-DGIGE_MEL_BUILD_ARAVIS=ON` fails with "requires exactly Aravis 0.8.36",
  and a `.pc` file declaring 0.8.33 fails with "found aravis-0.8 0.8.33, which
  is not qualified". The build logic was not changed.

### Remaining limitations

Everything here is software evidence against the real Aravis 0.8.36 library
and its in-process Fake device:

- The malformed native values in A are either native-snapshot/profile
  regressions or values imposed through the SDK's own property nodes. No
  physical camera produced them.
- The query failures in C are genuine SDK `GError`s from GenICam nodes added
  through Aravis's public DOM API.
- The SDK is not sanitizer-instrumented.

Image acquisition, clock mapping, physical-camera qualification and a second
real SDK remain future work. The two deferred PR #3 notes remain separate.

## GIGE-MEL-008 optional Aravis camera-control foundation (2026-10-30)

Branch `feature/gige-mel-008-aravis-control` was created from freshly fetched
`origin/main`, which had not advanced since task preparation:

- Commit `039de755644ad2b15dd5827878ae71c028218e8f`
- Tree `01476757e59fec54f66de1f3fa0820f0429b7375`

**Environment.** Linux 6.12.113+deb13-amd64, GCC 14.2.0, Clang 19.1.7, CMake
3.31.6, Ninja 1.12.1, Python 3.13.5. Local provisioning used meson 1.5.2 in a
virtualenv and Debian 13 GLib 2.84 / libxml2 development packages unpacked into
a user-space sysroot (no root access). No camera or network discovery was used.

**SDK identity.** Aravis **0.8.36**:

- Archive `aravis-0.8.36.tar.xz` from the GitHub release. SHA-256
  `246deaa0042a387ff1bc00332d0fb80537ce14abde2c28d1a54c91f17adc51bf` was
  verified, and tag `0.8.36` resolves to commit
  `97ff5e330919d7a44b871df4eda669feeafdf342`.
- Built by `scripts/provision_aravis.sh` into the temporary prefix
  `/tmp/gm8/aravis-prefix`: release build, viewer, GStreamer, USB3, packet
  socket, introspection, docs and tests disabled, library target only. The
  SDK was **not** sanitizer-instrumented.
- Every test binary prints the library actually loaded:
  `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`, runtime 0.8.36 and compiled
  0.8.36. `ldd` confirms the same path. The tests fail if the runtime version is
  not 0.8.36.

| Configuration | CTest | Provider | Backend | Portability | Aravis adapter | MEL probe | Diagnostic `--help` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GCC dev, SDK-free (`build/dev`) | **7/7** | 74/74 | 8/8 | 12 + 2 | — | — | — |
| GCC Release, SDK-free (`build/release`) | **7/7** | pass | pass | pass | — | — | — |
| GCC dev + Aravis (`build/aravis-dev`) | **10/10** | 74/74 | 8/8 | 12 + 2 | 10/10 | pass | pass |
| GCC Release + Aravis | **10/10** | pass | pass | pass | 10/10 | pass | pass |
| Clang 19 Debug + Aravis (`CXX=clang++-19`) | **10/10** | pass | pass | pass | 10/10 | pass | pass |
| ASan/UBSan + Aravis (`detect_leaks=1:halt_on_error=1`) | **10/10** | pass | pass | pass | 10/10 | pass | pass |
| TSan + Aravis (`halt_on_error=1`) | **10/10** | pass | pass | pass | 10/10 | pass | pass |

The SDK-enabled build adds exactly three CTest entries: `aravis_adapter`,
`aravis_mel_embedding` and `aravis_diagnostic_help`. The seven default entries
and their counts are unchanged. `ldd` showed `libasan.so.8` plus
`libubsan.so.1`, and `libtsan.so.2`, in the respective adapter test binaries.
The sanitizers instrumented the project's adapter, tests and provider only, not
Aravis, GLib or libxml2. No sanitizer startup or environment limitation
occurred.

Other checks:

- **Offline verifiers.** Both pass (97 MEL files; 2 JSON files). Neither
  manifest was changed.
- **SDK-free build.** The default build has no Aravis, pkg-config or GLib
  entries in `CMakeCache.txt`/`build.ninja`, and `ldd` of the provider and tests
  shows no Aravis or GLib. Synthetic capture works (64×48 PGM).
- **Install and consumer.** The default install and the separate installed
  consumer pass. Installing from the SDK-enabled build produces the identical
  non-documentation file set. No Aravis target, header, library or symbol is
  installed; only documentation mentions Aravis. A consumer configured with
  `PKG_CONFIG_PATH` unset builds and runs against that installed tree.
- **Whitespace.** `git diff --check` passes over the PR range.

### Controls

1. **SDK absence.** With `PATH=/usr/bin:/bin` and `PKG_CONFIG_PATH` empty, the
   default configuration succeeds. Its cache contains only the OFF option and
   no pkg-config/Aravis lookups. `-DGIGE_MEL_BUILD_ARAVIS=ON` fails at configure
   time with: "requires exactly Aravis 0.8.36; aravis-0.8 (or one of its
   GLib/libxml2 dependencies) was not found by pkg-config. Provision it
   explicitly …". A `.pc` file declaring 0.8.33 fails with "found aravis-0.8
   0.8.33, which is not qualified".
2. **Readback honesty.** Two mutations were made in a disposable copy
   (`/tmp/gm8/ctl`) against the same provisioned SDK:
   - **A.** `frame_rate_from_native` was made to round fractions, removing the
     4-ULP check. `aravis_adapter` failed in "SDK quantization and truthful
     readback" at `!thirty && … unsupported`, because 30.000300003 Hz became
     30 Hz. `aravis_mel_embedding` also failed, because the unrepresentable
     30 Hz command was accepted.
   - **B.** `apply()` returned the cached request instead of the readback. The
     same scenario failed at `quantized.value().exposure == 1'250'000ns`, since
     1 250 500 ns was falsely reported.

   With the implementation restored, `src/` in the copy is identical to the
   branch and the scenario passes.

No starting-main feature failure is claimed, because main has no optional
Aravis target. No broad mutation or repetition campaign was run.

### Recorded pinned-SDK behavior (Fake_1)

- Rates of 25, 40, 50, 1 and 1000 Hz read back exactly.
- 30 Hz gives period 33333 µs and reads back as `30.000300003000032` Hz. A
  period of 142857 µs reads back as `7.0000070000069998` Hz.
- `ExposureTimeAbs` 1250.5 µs reads back as 1250 µs.
- Range checking rejects `Width` 5000 with "greater than allowed maximum
  '2048'".
- Aravis's own `open_device` returns NULL with no GError for an unknown ID.
- The discovery strings change pointer on each refresh.

### Limitations

This is a camera-control foundation exercised against the real SDK and its
in-process Fake device. It is not an image backend: `create_stream()` is
unsupported. It is not a supported Aravis MEL profile, and it is not
qualification of GigE Vision, GenICam compatibility or any physical camera,
firmware, network or timing. The late-write failure is an SDK constraint
change arranged through `arv_gc_float_impose_max` from a private hook, not a
camera fault. Hosted CI results for the final head are recorded on the PR. The
two deferred PR #3 notes remain separate and untouched.

## GIGE-MEL-007 backend portability (2026-10-30)

Branch `test/gige-mel-007-backend-portability` was created from freshly fetched
`origin/main`, which had not advanced:

- Commit `14326c899eadd91417d3f48c0654d604eac41a51`
- Tree `cf9f5d8635e09497f7aa607429e6fcb9a6f23933`

Environment: Linux 6.12.113+deb13-amd64, GCC 14.2.0, Clang 19.1.7
(`clang++-19`), CMake 3.31.6, Ninja 1.12.1 and Python 3.13.5. No camera,
network or SDK was used.

**Production is unchanged.** The PR does not touch `src/`, `include/`, the 99
pinned dependency files, the factory signatures or the five-job workflow. The
new evidence qualifies the existing implementation. Added:

- `tests/support/contract_fixture_camera.{hpp,cpp}`: the independent test-only
  backend;
- `tests/provider_backend_contract_tests.cpp`: the shared scenarios;
- one CTest registration inside `BUILD_TESTING`;
- documentation in [backend portability](backend-portability.md).

| Configuration | CTest | Existing provider | Existing backend | New portability suite |
| --- | --- | --- | --- | --- |
| GCC dev | **7/7** | 74/74 | 8/8 | 12 shared runs (2 cases × 6) + 2 fixture checks |
| GCC Release | **7/7** | 74/74 | 8/8 | 12 + 2 |
| ASan/UBSan (`detect_leaks=1:halt_on_error=1`; `halt_on_error=1:print_stacktrace=1`) | **7/7** | 74/74 | 8/8 | 12 + 2 |
| TSan (`halt_on_error=1`) | **7/7** | 74/74 | 8/8 | 12 + 2 |
| Clang 19.1.7 Debug (`CXX=clang++-19`, separate `build/clang-dev`) | **7/7** | 74/74 | 8/8 | 12 + 2 |

CTest now has **seven** entries. The six previous ones are unchanged, and the
new one is `provider_backend_portability`. The test `pthread_join` forwarding
target resolved to `libasan.so.8` and `libtsan.so.2` respectively, so both
sanitizer runtimes were active. There were no sanitizer initialization
failures and no reduced settings.

Other checks:

- **Release build.** The first Release build of the new test failed because
  GCC 14 at `-O3` reported a `-Wfree-nonheap-object` false positive on a
  span-to-vector temporary. The test helper was rewritten (explicit copy plus
  an in-place `std::equal`); warnings were not relaxed.
- **Offline verifiers.** Both pass (97 MEL files and 2 JSON files).
- **Capture, install and consumer.** The synthetic capture works. The dev
  install and the separate installed consumer pass. Fixture references are
  limited to installed documentation; no fixture source, header, or symbol is
  installed.
- **Whitespace.** `git diff --check origin/main` passes over the whole change.
- **Bounded repetition.** Five dev runs each of the drain, fixture-direct and
  Health/BIT scenarios passed (15/15). Five TSan runs each of the drain and
  fixture-direct scenarios passed (10/10). Repetition supplements the
  deterministic join/gate/watchdog synchronization; it does not replace it.

### Starting-main and negative controls

- **Starting main.** An isolated detached worktree at `14326c8` was built with
  the dev preset; its own CTest passed 6/6. The provider DSO SHA-256 is
  `d0fec4bc…0b56`. The new test and fixture sources were compiled with the
  starting-main headers and linked to that DSO. `ldd` and `LD_DEBUG=files`
  (`calling init:`) confirmed `/tmp/gm7/base/build/dev/libgige_mel_provider.so.0`
  was loaded. Result: rc 0, 12 shared runs and 2 fixture checks passed. The
  existing provider needed no correction.
- **Mutation controls.** Each control is a disposable copy of starting main,
  one line changed in `src/provider.cpp`, with only the provider rebuilt. The
  same unchanged test binary was run against each mutated DSO using
  `LD_LIBRARY_PATH` (RUNPATH binary), and `calling init:` confirmed the loaded
  library each time.

| Control | Mutation | `[synthetic]` | `[contract-fixture]` |
| --- | --- | --- | --- |
| A (`3091dac6…`) | Row-by-row repack replaced by one `memcpy(destination, source, width*height)` | rc 0, 6/6 pass (tight rows hide it) | **rc 1**: `image publication and row repacking: pixel mismatch at x=0 y=1 frame=4294967280: got 165, model 127` (0xA5 padding leaked) |
| B (`d33b2e97…`) | Detailed `backend_id` reported as literal `"synthetic"` | rc 0, 6/6 pass | **rc 1**: `identity, instance lookup and discovery: values.at("gige_mel.backend_id") == kase.backend_id` |

The mutated copies were discarded, and final validation used the unmodified
provider. No broader mutation campaign was run.

### Limitations

Both cases are software backends. This record does not qualify a GigE Vision
SDK, physical camera, transport, packet behavior, disconnection, trigger
timing, clock mapping, PTP/UTC or calibration. It does not complete the
roadmap's "first real camera" or "second implementation" milestones. Hosted CI
results for the final head are recorded on the PR, not here. The two deferred
PR #3 notes remain separate and untouched.

## GIGE-MEL-003 configuration and command status (2026-10-28)

Starting freshly fetched main: commit
`d9d4a626ead7a0956702273ca292143164dc2ed5`, tree
`34b5546c5d225a64898de27d8653a1bee44df8d9`.
Linux x86-64, GCC 14.2.0, Clang 19.1.7, CMake 3.31.6 and Python 3.13.5;
all camera coverage uses synthetic sources or test-only decorators.

- GCC development, GCC Release and Clang Debug: CTest **6/6 passed** each.
- ASan/UBSan: **6/6 passed**, with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- TSan: **6/6 passed**, with `TSAN_OPTIONS=halt_on_error=1`.
- Native capture, install/export and separate installed-consumer build/run passed
  against GCC Debug, GCC Release and Clang Debug provider builds. Installed
  consumers exercise ConfigSet and status with no JSON headers installed.
- The provider suite retains all 29 previous groups and adds five configuration/
  status groups: schema/size/encoding and zero-apply rejection, shared transaction
  and ordering, every backend reason mapping and readback failure/mismatch,
  same/cross-provider reentry/throw/drain, and suppression/lifetime races.
  Existing fault-recovery coverage now also checks ConfigSet rejection/status.
- The complete development provider suite passed **100 consecutive repetitions**.
- Public-API negative control: `tests/configuration_probe.cpp` was compiled against
  an isolated starting-main library. With `--expect-unsupported` it confirmed valid
  ConfigSet returns Unsupported (with/without callback) and status registration
  returns NotSupported. Without the flag, the corrected feature expectation fails
  with exit 4. Against the feature, the CTest probe passes with correlated Accepted
  and both registered/unregistered ConfigSet success. No baseline runtime edits.
- Both offline dependency verifiers passed: all original 97 MEL files unchanged,
  plus two nlohmann/json files pinned separately. Parser source bytes were checked
  against the immutable upstream Git tree's blob identities before locking hashes.
- Final diff whitespace check passes; the immutable MIT license's original
  first-line trailing space is retained with a file-specific whitespace attribute.
- Workflow success criteria and the five hosted jobs are unchanged. Final-head
  hosted results are recorded in the PR/handoff, not inferred from local passes.

The exact schema, result/reason tables and callback/detach limitations are in
[configuration](configuration.md). No hardware, network transport, SDK, UTC/PTP,
automatic recovery or broader MEL conformance is claimed.

## GIGE-MEL-001 callback correction (2026-10-28)

The callback correction was validated locally on Linux x86-64 with GCC 14.2.0,
Clang 19.1.7, CMake 3.31.6, Ninja, and Python 3.13.16. This supplements the
historical bootstrap record below; all camera tests still use synthetic sources.

- GCC development, GCC Release, and Clang development builds: CTest **4/4 passed**
  in each configuration, including native loading and upstream integrity.
- ASan/UBSan: CTest **4/4 passed**, with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- TSan: CTest **4/4 passed**, with `TSAN_OPTIONS=halt_on_error=1`.
- Capture, installation, and separate installed-consumer build/run passed for
  GCC development, GCC Release, and Clang development provider builds.
- The provider suite now has **24 test groups**. Its three new groups cover the
  gated A-to-B nested communications callback versus external A detach,
  same/cross-provider mutation rejection in image and metadata callbacks, and
  nested metadata exceptions restoring callback context. Existing buffer lease,
  external drain, callback exception, and last-Control-reference tests pass.
- The development provider suite passed **100 consecutive repetitions**.
- Negative control: the new tests were copied into a disposable worktree at
  `81d5023eb9b7dde964bf56b666037fc338b26012` without the runtime correction.
  The first test established admission closure through detached keep-alive,
  released the nested callback gate, and failed with CTest's **30-second timeout**.
  With the correction the entire provider suite completes in about 0.33 seconds.
- `python3 scripts/vendor_upstream.py --verify` verified all 97 pinned files;
  `git diff --check` passed. No workflow, pinned header, public ABI, or backend
  interface changes were needed.

The five existing hosted jobs remain unchanged. Their results for the delivered
PR head are recorded in the PR/handoff, not inferred from these local runs.

## Executed checks

The bootstrap was compiled and exercised on Linux x86-64 with GCC 13.3.0,
CMake 3.31.6, Ninja, and Python 3.12. The code requires C++20; project warnings
were treated as errors. All checks below used the synthetic backend or an
explicit test decorator, not camera hardware.

| Check | Result | What it establishes |
| --- | --- | --- |
| Debug configuration and build | Passed | Provider, backend, example, and all test executables compile together against the pinned headers |
| Development CTest | **4/4 passed** | Backend contracts, provider contracts, standalone native loader, and upstream integrity |
| AddressSanitizer + UndefinedBehaviorSanitizer CTest | **4/4 passed** | The exercised paths completed without detected address or undefined-behavior errors |
| LeakSanitizer | **Enabled and passed** in the successful ASan run | No detected leaks in the exercised process lifetimes |
| ThreadSanitizer CTest | **4/4 passed** | No detected data races in the exercised concurrent paths |
| Standalone capture executable | Passed | Dynamically loaded provider delivered a 64×48 Mono8 frame and the client wrote binary PGM |
| Install/export package | Passed | Headers, libraries, notices, documentation, and CMake targets install successfully |
| Separate installed consumer | Passed | An independent `find_package(GigeMelProvider 0.1 CONFIG REQUIRED)` project compiles, links, initializes, and discovers the provider |
| Upstream byte integrity | Passed | All 97 locked upstream files match their provenance/checksum manifest |
| Project whitespace check | Passed | Newly authored tracked content has no Git whitespace errors; unchanged upstream files are excluded from style editing |

The four CTest entries are suite executables/checks, not four total assertions:
`backend_contract` contains **8 test groups**, `provider_contract` contains
**21 test groups**, and `native_loader` exercises factory resolution and complete
image delivery/teardown through the actual shared library.

The successful sanitizer runs used normal process inspection. A preliminary
LeakSanitizer attempt inside the restricted execution sandbox could not inspect
`/proc` and did not complete; the subsequent run with process inspection enabled
completed with **`detect_leaks=1`**. No leak check was disabled in the successful
validation result above.

## Backend coverage

The eight backend groups cover:

1. Stable device discovery, unknown-device rejection, and exclusive open.
2. Complete configuration validation, actual readback, and rejection while running.
3. Mono8 frame metadata, deterministic pixels, and retained-frame stability.
4. Bounded pool exhaustion, release recovery, and stream/configuration generations.
5. A blocked receive waking when a frame lease is released.
6. A blocked receive waking when the stream is stopped.
7. Device-close cancellation and frame bytes surviving complete backend teardown.
8. Move-only lease transfer and ownership release.

## Provider coverage

The 21 provider groups cover:

1. Factory validation, idempotent initialization, discovery, and channel identity.
2. Null/undersized/duplicate/overlapping/foreign registrations, flags, and mutation of bound buffers.
3. Retained MEL slots, starvation, explicit release, and delivery recovery.
4. Disable waiting for an active callback and preventing further image publication.
5. Delivered bytes and explicit release surviving channel detach.
6. Complete camera snapshots, response correlation, rejected inputs, and disabled-only configuration.
7. Geometry growth requiring sufficiently large registered memory.
8. Supported metadata registration, duplicate/empty registration, reentry, and C2-to-Image response routing.
9. Image callback mutation rejection without deadlock.
10. Unsupported command and callback results using actual MEL enums.
11. A throwing Image callback stopping acquisition while preserving its explicit buffer lease.
12. Control destruction detaching externally retained channels.
13. Destruction of an old detached channel preserving the replacement stream's active state.
14. Dropping the last external Control reference during an Image callback while another thread disables.
15. Detach draining an admitted metadata callback and closing subsequent admission.
16. A test backend producing padded rows and the provider publishing tightly packed rows.
17. Rejection of incomplete frames, stale configuration generations, and incompatible clock domains.
18. An invalid first frame not poisoning the accepted stream generation.
19. Clock generation changes and backward timestamps stopping publication.
20. A partially applied camera command reporting an error while retaining and using actual device readback.
21. Failed configuration readback blocking acquisition even with sufficiently large buffers.

The padded/faulting adapter is a test decorator around the synthetic backend.
It tests the abstraction boundary; it is not a second GigE SDK implementation.

## Native ABI and package coverage

`loader_tests` includes only upstream headers and links only system thread and
loader support. It resolves all three upstream factories at runtime, rejects
bad instance/manager combinations, registers caller-owned memory, validates
three frames, explicitly releases each delivered slot, and destroys the complete
provider object graph before unloading the DSO.

`tests/install_consumer` is configured as a separate project against an installed
prefix. It verifies exported target dependencies, public include paths, shared
library loading, the standard factories, initialization, and discovery without
using source-tree include paths.

## Reproducing the main checks

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev

cmake --preset asan
cmake --build --preset asan --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan
```

Use the install and capture commands in [README](../README.md) for those checks.
Sanitizer initialization depends on host process/memory-mapping capabilities;
record an initialization failure separately from a detected program defect.

## What this validation does not establish

No physical camera, Aravis, eBUS, pylon, or GenTL producer was used. Hardware
discovery, SDK compatibility, real network transport, bandwidth, packet resend,
physical unplug/reconnect, trigger timing, PTP/UTC mapping, calibration, and
mission-specific MEL behavior remain unqualified.

Clang and Release builds are configured in GitHub Actions but were not executed
locally in this bootstrap session. The GitHub workflow itself has not run because
no accessible remote repository was available. Configured CI is not reported as
a completed remote run. Tests and sanitizers cover the recorded scenarios; they
are not proof of complete MEL conformance or absence of all possible defects.
