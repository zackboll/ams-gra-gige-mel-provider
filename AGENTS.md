# Repository instructions

## Purpose and boundaries

This repository implements the AMS-GRA IR MEL **provider** in native C++20.
MEL means MFA Encapsulation Layer. It is not Matrox MIL. The separate
`ams-gra-mel-bridge` repository is a consumer bridge and is not a dependency.

- Preserve the exact pinned upstream C++ declarations and factory signatures.
- Do not introduce a provider C ABI or stable runtime SDK plugin ABI as part of
  ordinary backend work. Those are separate architecture decisions.
- Keep vendor SDK types and includes in their adapter implementation targets.
  `include/gige_mel/backend.hpp` remains independent of MEL and vendor SDKs.
- Keep the default build usable without cameras, networking, or proprietary SDKs.
- Never claim full MEL conformance from pure-virtual stubs. Unsupported behavior
  must return an actual upstream unsupported result and remain unadvertised.

## Work and validation

Read `README.md`, `docs/implementation-profile.md`, and the relevant architecture
section before changing lifecycle, configuration, or buffer behavior. Run the
smallest meaningful contract tests that cover a change. For provider/backend
changes, normally run the development CTest suite and the applicable sanitizer
tests. Distinguish executed checks from CI configured but not yet executed.

The typical commands are:

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev
```

Retained frame bytes must not be overwritten before explicit release. Stop must
wake a blocked receive, and callbacks must not run under the runtime operation
lock. Test failure paths, full pool conditions, callback reentry, and teardown
when changing these contracts. Avoid tests that merely repeat implementation
details without detecting a user-visible failure.

## Dependencies and scope changes

Do not edit files covered by `third_party/manifest.json`. Verify with
`python3 scripts/vendor_upstream.py --verify`. An upstream revision change needs
new independently verified provenance, a measured include closure, and matching
native client/provider validation. Preserve original licensing and intent files.

Update the implementation profile and surface roadmap when a capability changes.
Document which SDK/camera combinations were actually tested. Do not silently
emulate unavailable navigation, tracking, calibration, protection, or UTC time.

Keep generated binaries, images, SDK downloads, and build trees out of commits.
