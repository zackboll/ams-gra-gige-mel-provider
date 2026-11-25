# GIGE-MEL-049 — GenTL transport layout validation

Base: `23c58b063f70d899977e0b82eb391528472379b5` (fetched `origin/main`).
This is a source relocation, not new producer, camera, or MEL qualification.

## Source and build boundaries

`git mv` moved the three files from `src/gentl/` to
[`src/transport/gentl/`](../../src/transport/gentl/), with no forwarding headers.
`git diff --find-renames` and Git blob comparisons against the base gave:

| File | Similarity | Base → relocated blob |
| --- | --- | --- |
| `gentl_api.hpp` | 100% | `8d5ad29dccb197af595d9ac67098b215f86afff3` (identical) |
| `gentl_consumer.hpp` | 100% | `a5ac8ad0c60494a28973b6399e66dadbf322b079` (identical) |
| `gentl_consumer.cpp` | 99% | `34a393015c2a71cbd7cafc84028f1368d97f7179` → `3d50ce6755a788c59112a57b7bdd59902c586b0a` |

Removing `gentl/` from the two sibling includes in the original `.cpp` makes
it byte-identical to the relocated file. The probe, consumer test and private
backend header `gentl_genapi.hpp` now use the transport-relative include. With
the configured GCC flags, `-E -P` output matches the base byte-for-byte for
the consumer and both GenTL backend translation units. The backend implementation
and its read-only contract are unchanged. Remaining upstream Aravis 0.9.2 paths
in the provisioning script and GenTL design records refer to Aravis, not this tree.

The SDK-free `gentl_default_isolation` test now checks the layout and CMake
wiring. Negative controls rejected a missing header, a duplicate at the old
path, a wrong consumer target, a missing backend-to-consumer link, and an added
provider-to-consumer link. Generated Ninja rules confirmed the relocated source,
the backend's consumer dependency, and no GenTL targets in the default build.

## Local qualification

Fresh Debug builds used GCC 14.2 or Clang 19.1.7 with warnings as errors and
`GIGE_MEL_TEST_CONTROL_AUTHORITY=ON` for the CTest matrix. Test suites ran
serially across build directories. Sanitizers used halt-on-error, with leak
detection enabled for ASan. Both optional GenTL lanes used the provisioned
checksum-verified EMVA GenTL 1.6 header; the backend resolved the provisioned
Aravis 0.8.36 for GenApi and libxml2 2.9.14. No real producer or camera was used.

| Configuration | CTest |
| --- | --- |
| GCC Debug, GenTL off | 57/57 passed on full rerun |
| Clang Debug, GenTL off | 57/57 passed on full rerun |
| GCC ASan/UBSan, GenTL off | 57/57 passed |
| GCC TSan, GenTL off | 57/57 passed |
| GCC GenTL probe only | 98/98 passed, including 41 GenTL tests |
| GCC GenTL probe + backend | 117/117 passed, including 60 GenTL tests |

The first GCC and Clang Debug runs each failed only the unchanged
`gvcp_single_owner` fixture's `/proc/self/task` count assertion while other
lanes were building. Both full reruns after those builds finished passed
without code changes, test exclusions or retries within a suite.

The GenTL lanes exercise producer initialization and cleanup, exact selection,
port/register I/O, stream lifecycle, buffer ownership and quarantine,
cancellation, allocation and teardown failures, and the probe CLI. The backend
lane includes read-only contracts, failure injection, and the fixture example's
enumeration and profile-refusal cases. A separate probe-only build also compiled
the consumer and probe with `BUILD_TESTING=OFF` and the backend disabled.

## Installed surface and repository checks

- The GCC default, probe-only, and probe + backend installations matched the
  base's 127 header/library/package paths, all 119 header files and all four
  CMake package files byte-for-byte, and the inventories of 8,627 defined
  provider dynamic symbols and 706 external backend archive symbols.
- The provider SONAME, direct ELF dependencies and resolved library names were
  unchanged, with no CTI, GenTL or Aravis dependency. The installed exports
  remain `gige_mel::provider`, `gige_mel::backend` and `gige_mel::mel_headers`.
  The default and probe + backend installs both passed separately built native,
  MEL-only stacked-image and virtual-task consumers, and the installed capture
  example, without `PKG_CONFIG_PATH`.
- Doxygen 1.14.0 and site validation passed. Its sole diagnostic, the
  `with_registers` declaration matching warning, matches the base after
  normalizing the relocated paths. MEL traceability and the evidence index
  passed, including their negative controls. Upstream integrity (120 pinned
  files) and JSON integrity (two pinned files) passed offline. The proprietary-
  content guard and `git diff --check` passed.