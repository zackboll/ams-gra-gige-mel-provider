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

Proprietary SDKs must remain entirely outside the working tree. Never publish
SDK headers, source, libraries, samples, installers, EULAs, keys or SDK-dependent
build artifacts. Enable local safeguards with `git config core.hooksPath .githooks`.
Before committing, inspect staged files and run `python3 scripts/check_proprietary.py`;
before pushing also run it with `--history origin/main`. CI repeats the Git-blob
checks. See [Pleora integration](docs/pleora-integration.md) for licensing and
qualification restrictions. A successful download does not grant vendor rights.

## Documentation

Follow [the local Doxygen build instructions](README.md#build-the-documentation)
and open the generated landing page in `build/documentation/docs/html/`. Review
the installed (`include/gige_mel/`), camera extension (internal, uninstalled), and
core implementation (internal) sections. The target checks local links and API
entry points and writes warnings to `build/documentation/docs/doxygen-warnings.log`.
See the [warning baseline](docs/documentation-warnings.md) for known limitations.
Do not commit generated pages or treat extracted internals as stable API.

For a new camera profile, update [camera-profile authoring](docs/camera-profile-authoring.md)
or a linked model guide with feature/readback requirements, ownership, module
compatibility and qualification limits. For backend changes, keep the
[backend authoring contract](docs/backend-authoring.md) current and document
ownership, cancellation, error and thread semantics on the relevant C++
declarations. Put API comment improvements next to project declarations and
maintainer notes next to the relevant implementation; do not edit pinned upstream
headers. Use the [documentation index](docs/index.md) for new guide navigation.

For consumer-facing changes, check [Integrating a MEL Consumer](docs/consumer-integration.md)
against the provider contracts and build the [standalone example](examples/installed_consumer/)
against a freshly installed prefix. Keep it independent of `src/` and repository
test helpers; advanced behavior belongs in the focused contract tests and guides.

The [documentation workflow](.github/workflows/documentation.yml) builds the exact
PR head and validates without deploying. A successful `main` push build uploads
the same-run Pages artifact, then separate jobs deploy and verify the live source
commit and critical entry points. A repository administrator must set **Settings →
Pages → Build and deployment → Source → GitHub Actions** before the first deploy.

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
