# Dynamic profile module qualification

Base commit `1523b8b278fb864c539298943bd2100875caa151`, tree
`d72f68b9614bccdac268e2cc5d230b75ef4ea1e9`; PR #28 confirmed merged.
Branch `feature/gige-mel-035-dynamic-profile-loading`.
Final commit/tree and final-HEAD executed results are reported on the delivery
PR after normal commit creation (no self-referential commit identity here).

## Reproduction matrix

Linux x86-64, GCC 14.2.0 and Clang 19.1.7/libstdc++, CMake/Ninja Debug.
Build directories `build/035-<lane>` are ignored, logs `/tmp/035-final-<lane>.log`
and `/tmp/035-final-<lane>.result` are not committed.

| Lane | Aravis | Pleora | Sanitizers |
| --- | --- | --- | --- |
| free-dev | OFF | OFF | none |
| free-asan | OFF | OFF | address,undefined |
| free-tsan | OFF | OFF | thread |
| clang-free | OFF | OFF | none (Clang) |
| aravis-dev | ON | OFF | none |
| pleora-dev | OFF | ON | none |
| both-dev | ON | ON | none |
| both-asan | ON | ON | address,undefined |
| both-tsan | ON | ON | thread |

Configure each with `cmake -S . -B build/035-LANE -G Ninja
-DCMAKE_BUILD_TYPE=Debug`, explicitly setting optional adapter flags and
`GIGE_MEL_SANITIZERS`. Build `--parallel 2`, then full
`ctest --test-dir build/035-LANE --output-on-failure`. Native suites must run
serially across build trees because their simulated GigE fixtures share fixed
UDP port 3956; CTest resource locks do not synchronize different processes.
An exploratory concurrent run exposed this collision and was not accepted as
final evidence. An initial Clang unused negative-fixture variable warning was
fixed rather than disabling warnings-as-errors.

Runtime options: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`,
`TSAN_OPTIONS=halt_on_error=1`. Combined TSan uses the existing audited GenApi
single-function exception wrapper, unchanged. No unrelated reports are disabled.

Existing external SDKs only: Aravis 0.8.36 at
`/home/zboll/sdk/aravis-0.8.36` (pkg-config path `lib/pkgconfig`),
eBUS 6.5.3.7155 at `/home/zboll/sdk/pleora/ebus_sdk`, external matching GenApi
3.4.1 headers `/home/zboll/sdk/gige-mel-027-genapi/headers/library/CPP/include`.
No downloads or physical-camera runs. SDK binaries are not instrumented;
existing native handoff blind spots and scoped exception still apply.

## Independent module evidence

Every test build independently configures/builds `examples/profile_module` via
ExternalProject, using the same compiler/flags/sanitizers but no host target link.
`nm -D --defined-only` shows only `gige_mel_camera_profile_module_v1`.
`readelf -d` shows no provider, Aravis, Pleora or GenApi dependency and no NODELETE.
The module is not among host executable/provider DT_NEEDED dependencies.
An additional standalone example configure/build and invocation against the
already built host prove the shared-library loading boundary without rebuilding
provider/backend targets. Exact commands are in the example README.

## Behavioral and lifetime contracts

* `dynamic_profile_module_contract` is its own test process. It checks missing,
  relative and NUL paths, missing symbol, unsupported API/interface versions,
  wrong size/build ID, null descriptor/factory/destructor, malformed ID, missing
  compatibility, mismatched returned ID, factory expected error, null result,
  standard/unknown unexpected exception and erroneous error+object cleanup.
  Incompatible fixture factories abort if invoked, proving checks precede calls.
  All failed mappings have observed ELF finalization; plugin finalizers abort if
  profiles/states remain alive. Duplicate registration, independent factories,
  failed device compatibility open, no fallback and 20 repeated independent-session
  load/open/destroy cycles are covered. States remain callable after profiles and
  catalog destruction; actual unload occurs only after the last state is released.
  Host exception unwinding also observes finalization.
* `aravis_adapter` repeats source-profile and dynamic-profile scenarios against
  the actual Fake camera. Both select `test_rate_limited_v1`; >20 Hz rejects
  before writes, 20 Hz succeeds with actual readback, native query errors retain
  their native message. Catalog destruction precedes device open/destruction.
* `aravis_stream_contract` adds the dynamically loaded module to unchanged native
  receive. Odd IDs reject, even IDs deliver. Teardown observes no unload while
  profile/state owners remain and actual unload after their final retirement;
  retained native-frame bytes are still readable and unchanged after unload.
* `pleora_sdk_contract` executes both source and dynamic versions in its existing
  named eight-group native audit. Real GenApi Features run policy and preserve
  Pleora/native error provenance. The production SDK-buffer copy stream executes
  frame policy after catalog, backend, profile and explicit state destruction;
  its validator alone pins the module. Stream retirement triggers actual unload
  while a copied frame is retained, whose bytes remain unchanged. This is NOT a
  connected PvDeviceGEV, transport, entitlement or physical-camera qualification.
* `builtin_selection_contract` and CLI tests validate explicit compatibility,
  unavailable backend rejection, duplicate module registration, no synthetic
  fallback, no-I/O registration/selection, and preserved built-in behavior.

## Additional final checks

Final qualification includes SDK-free and combined install/standalone consumers,
the checked package-isolation verifier, independent Pleora/profile dependency and
symbol checks, upstream/JSON byte integrity, index/new-history publication guard,
documentation link and example compilation checks, and `git diff --check`.
Executed results (including any blocker) belong in the PR qualification matrix;
this reproduction plan does not claim an unexecuted check passed.

No installed provider/MEL factory, backend implementation, payload, time mapping,
navigation transport, sanitizer suppression, SDK pin or third-party byte changes.
Matched-C++ compatibility metadata is not authentication or a universal ABI.
No Windows, search-directory discovery, forced unload, hot replacement, runtime
backend plugins or physical-camera qualification is implemented.