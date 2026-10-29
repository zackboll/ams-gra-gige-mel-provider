# Bootstrap validation record

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
