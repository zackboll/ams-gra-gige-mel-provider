# Contributing

Start with the quick start in [README](README.md). The development build needs
Linux, a C++20 compiler, CMake 3.24 or newer, Ninja, and Python 3.9 or newer. No
camera or SDK is required. Original project sources use Apache-2.0; upstream
declarations retain their own original notices under `third_party/`.

Keep changes scoped to one observable behavior. Explain the problem, the new
behavior, and validation in a pull request. If a change alters supported MEL
surfaces, update [the implementation profile](docs/implementation-profile.md)
and [the roadmap](docs/mel-surface-roadmap.md). If it alters memory ownership,
clock semantics, or shutdown, include a focused regression test.

## Adding a backend

Read [backend authoring](docs/backend-authoring.md). Begin with the normal copy
path and reuse the common provider. Add an optional SDK-specific CMake target,
an explicit device selector, and a reproducible SDK/camera test description.
Do not put SDK headers in the common backend contract or require commercial
software for the synthetic CI jobs. A second real adapter should exercise the
same provider without editing MEL channel code.

## Dependency maintenance

Ordinary builds verify checked-in upstream bytes and perform no downloads.
`scripts/vendor_upstream.py --restore` is an explicit maintenance command that
restores the currently locked revisions; it does not upgrade them. Read
[upstream provenance](docs/upstream-provenance.md) before changing the pins.

## Tests and diagnostics

Use `ctest --preset dev` for normal development and `ctest --preset asan` after
building that preset for address/undefined behavior instrumentation. The `tsan`
preset is available for race analysis on a compatible host. Do not suppress a
reported defect to make a job green. If the runtime cannot initialize inside a
container, record that environment failure separately from test failures.

Hardware reports should include the commit, backend, SDK version, camera model,
firmware, pixel format, ROI, trigger mode, frame rate, buffer counts, and clock
configuration. Synthetic test success is not evidence of packet-loss recovery,
physical disconnect handling, PTP synchronization, or hardware calibration.
