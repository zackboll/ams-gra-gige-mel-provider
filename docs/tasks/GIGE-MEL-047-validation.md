# GIGE-MEL-047 — SDK-free qualification

**Scope:** the [optional profile navigation handler](GIGE-MEL-047-design.md),
its bridge, an SDK-free recording backend, the unchanged MEL entry point and
separately built profile modules. **Disposition: `PROFILE_NAVIGATION_FIXTURE_QUALIFIED`.**
No physical camera, native SDK, socket, register write or transport was used or
qualified. Success in these tests means *recording-port acceptance* only: no
transport acknowledgement, detector acceptance, image association or transfer
alignment.

## Source and environment

Fresh `origin/main` baseline: `076548635ed664434a9746a22b82c9aad95f7e3b`
(merge of PR #59). The exact delivery commit and tree and the hosted results are
recorded on the PR, not inferred from the baseline or from CI configuration.
The run used Debian Linux x86-64 on 2026-11-20 with CMake 3.31.6, Ninja 1.12.1,
GCC 14.2.0 and Clang 19.1.7. `BUILD_TESTING=ON`,
`GIGE_MEL_TEST_CONTROL_AUTHORITY=ON` (the CI setting), and all SDK, producer
and hardware options off. Each lane used a fresh build tree:

```sh
cmake --preset dev -DGIGE_MEL_TEST_CONTROL_AUTHORITY=ON
cmake --build --preset dev --parallel 4
ctest --preset dev
ctest --preset dev --repeat until-fail:20 -R \
  '^(profile_navigation_|profile_identity_closure|dynamic_profile_module_contract|provider_contract|control_authority_)'
```

Release, ASan/UBSan and TSan used the `release`, `asan` and `tsan` presets.
Clang Debug used `CXX=clang++ cmake --preset dev -B build/clang`. Sanitizer
settings: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, `TSAN_OPTIONS=halt_on_error=1`.
No suppression was added or used. The external profile modules are rebuilt by
`ExternalProject` in each lane with that lane's compiler, flags and sanitizers.

## Results

| Lane | Complete SDK-free CTest | Focused repetitions (14 tests) |
| --- | --- | --- |
| GCC Debug | 50/50 passed | 20 each, all passed |
| GCC Release | 50/50 passed | in suite |
| Clang Debug | 50/50 passed | in suite |
| GCC ASan/UBSan | ASAN_RESULT | ASAN_REPEAT |
| GCC TSan | TSAN_RESULT | TSAN_REPEAT |

The suite grows from 40 to 50 tests: nine `profile_navigation_*` scenarios and
`profile_identity_closure`. The focused set is those ten plus the unchanged
`provider_contract` (existing navigation forwarding),
`dynamic_profile_module_contract`, `control_authority_contract` and
`control_authority_stale_gvcp_ack`. Existing tests were neither replaced nor
relaxed, and the GIGE-MEL-046 sources and tests are byte-identical.

### Scenario evidence

All scenarios are in [`profile_navigation_tests.cpp`](../../tests/profile_navigation_tests.cpp).
Expected values are specified independently of the provider's conversion code,
and records are decoded field by field.

| Scenario | Observed result |
| --- | --- |
| `bridge_capability_discovery` | Built-in `mono_free_running_v1` and test `test_rate_limited_v1` states expose no handler, and bridge creation returns `unsupported`. `test_navigation_record_v1` exposes it and keeps ordinary capabilities. Null inputs are `invalid_argument`. Refused creation invokes no handler and retires each offered port. |
| `bridge_preparation_and_mapping` | FreeInertial/GPS/Blended produce exact fictional records, with the report copy equal to the input. Aligning is `unsupported`, an excess variance is `invalid_argument`, and injected error, exception, allocation, empty-format and altered-report cases all fail **before** the port (no port event). A definite port refusal is not sticky, and an explicit resubmission succeeds. |
| `bridge_uncertainty_and_control_loss` | Uncertain completion and a port exception after recording both return `timeout`; the effect stays recorded and the next submission is refused with neither handler nor port entry. Acceptance with control loss returns `timeout`, then `disconnected`. Loss before send returns `disconnected` with no operation. |
| `bridge_concurrency_and_teardown` | A concurrent submission is `busy` (no queue). Reentrant submit and shutdown from the handler are refused without a state change. 25 cycles, with the last external profile/state reference dropped before submission: shutdown is still pending at 20 ms while the port is held, nothing is retired, then the submission completes, port and state retire in order, and later submits are `disconnected`. |
| `mel_profile_a_receipt_only` | Through `make_api_manager` → `init` → Image attach → `send`, both Profile A policies on the same generic backend expose no sink: zero ports, ready echo and callback, Aligning accepted (no profile policy), stale rejection. |
| `mel_profile_b_dispatch` | Exact preservation for three states, with event order `sink entered → port recorded → callback` (callback after acceptance, zero IDs, exact time echo). Invalid MEL reports (latitude, default report) and stale timestamps never enter the handler. Profile Unsupported / InvalidParameters, handler error → InsufficientRemoteResources, exception → InvalidState, allocation → InsufficientLocalResources, altered report → InvalidState: no operation or callback, no timestamp commit, and the same time succeeds afterwards. Port disconnect → InsufficientRemoteResources. Provider reentry from the handler is refused without deadlock. |
| `mel_profile_b_uncertainty` | Uncertain, port exception after recording, and acceptance with control loss each give MEL `InvalidState` "uncertain" with no callback. The next send does not enter the handler, which means no automatic retry and no later success: InvalidState, or InsufficientRemoteResources after loss. |
| `mel_profile_b_lifecycle` | Detach waits for an admitted submission, the old channel is then detached, and reattach admits a fresh timestamp threshold. With frames streaming, at least two frames are delivered while a submission is held in the port. 10 cycles dropping manager, Control and channel references during an admitted submission: it completes, then the events are `port retired` → `device closed`, with exactly one state retirement. |
| `dynamic_module_navigation` | See below. |

### Dynamic modules

The modules come from the separately configured `examples/profile_module`
project (not linked into the host), built with the matched toolchain.
`ModuleUnloadProbe` counts actual ELF finalization.

* The incompatible-identity variant is refused as "build identity" before its
  factory, which would abort if called. It unloads.
* `librate_limited_profile.so` (no extension): the host-visible state has no
  handler, and the bridge returns `unsupported`.
* `libnavigation_record_profile.so`, 10 cycles: the wrapped state exposes the
  handler. After the probe, factory, selected profile and catalog are released,
  the module stays mapped. The module handler prepares the exact record; a
  rejected Aligning preparation keeps the pin. Shutdown racing an admitted
  module-handler submission stays pending with the module mapped; after release,
  unload occurs exactly once. Recorded operations stay valid after unload, as
  plain values.
* Full MEL path with the loaded profile: success and profile Unsupported. The
  module unloads only when the session is destroyed.

### Mutation checks (local, not committed)

Each deliberate fault was applied temporarily, rebuilt, and observed to fail
the named scenarios. The source was then restored and the suite rerun green:

| Fault | Detected by |
| --- | --- |
| Loader always returns the plain wrapper | `dynamic_module_navigation` |
| Uncertain completion mapped to success | `bridge_uncertainty_and_control_loss`, `mel_profile_b_uncertainty` |
| Shutdown does not drain the active submission | `bridge_concurrency_and_teardown`, `dynamic_module_navigation` (crash) |
| Altered report copy accepted | `bridge_preparation_and_mapping`, `mel_profile_b_dispatch` |
| Uncertainty not sticky | `bridge_uncertainty_and_control_loss`, `mel_profile_b_uncertainty` |
| `navigation_handler.hpp` removed from the identity closure (copied tree) | `profile_identity_closure` |

## Compatibility and publication checks

| Check | Result |
| --- | --- |
| `ICameraProfile` / `ICameraProfileState` vtables (GCC `-fdump-lang-class`, before vs after) | Identical: 6 and 10 entries, same order |
| `module::Descriptor` v1 layout | Identical, 864 bytes; discovery symbol and `interface_version` 1 unchanged |
| Byte identity of `include/gige_mel/*`, `camera_profile.hpp`, `profile_module.hpp`, `frame_policy.hpp`, `device_features.hpp`, `src/provider.cpp`, `src/control/*`, Cognex module | Unchanged (SHA-256) |
| Release `libgige_mel_provider.so` vs baseline | Identical exported symbols, `NEEDED` entries and `.text` bytes |
| Installed headers | Same four headers, identical to source; no new installed file |
| `profile_identity_closure` (+ negative control in a copied tree) | A comment-only change to `navigation_handler.hpp` changes the identity; removing the header from the closure fails the test |
| Baseline-built (pre-047) `librate_limited_profile.so` loaded by this host | Refused: "incompatible C++ build identity" |
| Module exports (`nm -D`) | Only `gige_mel_camera_profile_module_v1` |
| Installed consumer, standalone installed example, installed stack consumer, synthetic `mel_capture` | Passed (receipt-only navigation unchanged for the default synthetic backend) |
| Doxygen 1.14.0 site build and validation | 891 pages, 69 source browsers (both new example files included), search, 48 authored pages in the unchanged hierarchy; 16 internal types and 20 member descriptions under Internal Camera Extensions. The single warning is pre-existing (`gentl_consumer.cpp:865`), identical to the baseline |
| MEL traceability self-test, qualification-evidence index, pinned upstream (100 files), private JSON, proprietary guard (index and `--history origin/main`), `git diff --check` | Passed |

The navigation types are internal and uninstalled. No new top-level homepage
section was added; the new types are documented under **C++ API Reference →
Internal Camera Extensions**, and existing generated page URLs are unchanged.

## Not executed and remaining gates

No physical-camera, native SDK (Aravis, Pleora, GenTL) or optional adapter
test was run locally, and none is affected: no production device object
implements the sink, and none links `gige_mel_profile_navigation`. Hosted CI and
independent technical review are merge gates, and their results are reported
on the PR.

Physical detector navigation remains blocked on: qualified exclusive control
ownership with a production executor grant (GIGE-MEL-046 has none); bounded,
cancellable transport completion with cross-session stale-ACK fencing; a
detector-defined encoding, staging, commit and acceptance protocol, with
duplicate and recovery policy; authorized hardware evidence; and separate
image-association and transfer-alignment qualification. `PRODUCTION_CONTROL_QUALIFIED`
and `NAVIGATION_TRANSPORT_READY` are **not** claimed.
