# GIGE-MEL-048 — Backend layout validation

The [design](GIGE-MEL-048-design.md) records the baseline and the ownership
boundary for each backend. No physical-camera or real GenTL producer run is part
of this qualification.

## Focused regression checks

| Stage | Executed checks |
| --- | --- |
| Synthetic move | Byte comparison against the original source, `backend_source_layout`, `backend_contract`, `provider_backend_portability` (3/3) |
| Aravis feature extraction | `aravis_adapter`, `aravis_register_contract` (2/2) |
| Aravis stream extraction | Adapter and register checks plus `aravis_stream_contract`, `aravis_timestamp_contract` and `aravis_gige_loopback_contract` (5/5) |

The full lanes rerun the focused contracts after integration. Local CTest runs
use one test process at a time and share a cross-build lock so the loopback
fixtures do not contend for UDP port 3956.

## Contract evidence and fixture limits

| Contract | Aravis | Pleora |
| --- | --- | --- |
| Construction and exact selection | Lazy interface and native-open counters; exact discovered ID in Fake and GigE loopback | Catalog/profile construction with no profile open; malformed MAC, IP and prefixed aliases refused before discovery. Positive exclusive eBUS open requires a licensed native camera and is not claimed here |
| Access, capabilities and configuration | SDK Fake GenICam, fresh readback, typed errors and ordered writes; shared selected-profile assertions | Native eBUS result translation and GenApi node map over a local XML/port fixture; shared selected-profile assertions, live bounds and native read failures |
| Acquisition preflight and validation | Fresh profile preflight, native command/receiver lifecycle, Fake pixels and separate decoded-facts fault coverage | Selected profile preflight exercised through native features; production stream validation on controlled, real SDK-allocated `PvBuffer`s. This does not prove native stream enable or network retrieval |
| Cancellation, close and leases | Cancel/stop and logical close reject later work; lease retains its popped SDK buffer through teardown without retaining the retired receiver | Controls prove cancellation without relying on stop; native register operations on a disconnected device reject I/O. Controlled stream leases preserve every byte after native buffer overwrite and source destruction. No connected eBUS close claim |
| Shared policy lifetime | Selected in-tree and dynamic profiles, including state retention after device close and module unload while a lease remains readable | Same selected teaching profile with native feature and controlled receive fixtures, state pinned by the validator, and module unload independent of the copied lease |

`provider_backend_portability` still exercises the unchanged synthetic and
independent SDK-free provider fixture. Its MEL destination ownership evidence
does not substitute for the separate adapter source-lease checks above.

## Installation and source checks

- Separate installed C++ consumers, MEL-only Stack and virtual-task consumers
  built and ran from SDK-free GCC and Clang, Aravis-only, Pleora-only, combined,
  and GenTL fixture installs. The standalone installed synthetic capture also ran.
- The package-isolation verifier checked Aravis-only and combined builds: the
  native adapter loaded pinned Aravis 0.8.36; the installed provider did not
  depend on it and the external consumer built without `PKG_CONFIG_PATH`.
  The combined SDK build and install also passed with `BUILD_TESTING=OFF`.
- Against the original `5d3f585` install, 119 header files match byte for byte,
  as do the CMake package exports. The SDK-free backend archive retains all 706
  external defined symbols. Provider SONAME, direct ELF dependency entries,
  resolved dependency names and factory exports match, with no vendor dependency.
  The raw provider dynamic-symbol list has three additional weak
  `std::vector<ChannelCapability>` swap implementation symbols. A separate clean
  install of `199787d` (the integrated `main`, after PRs #62 and #63 landed)
  has exactly the same list, including those symbols. GCC installs from the
  candidate's default and optional builds match that baseline's 8,627 provider
  symbols and all 127 non-documentation install paths.
- The source-layout check rejects both a duplicate at the retired location and
  registration on the wrong CMake target. A token comparison matched the moved
  typed-access and acquisition function bodies, including native error mapping,
  start, receive, retirement and logical close. The synthetic file is unchanged.
- Upstream and JSON integrity, the documentation and traceability validators,
  evidence-index checks, proprietary-content guard and `git diff --check` passed.
  Doxygen reports the existing GenTL `with_registers` warning and no new warning.

## Integrated qualification

The refactor started at `5d3f585`. Both subsequent `origin/main` updates were
integrated by normal merges after PRs #62 and #63 landed. The integrated builds
include the newly landed discovery-publication test. GCC 14.2 and Clang 19
Debug, SDK-free ASan/UBSan, and SDK-free TSan each passed 56/56. Aravis-only
passed 64/64, Pleora-only 62/62, and combined Debug 70/70. SDK runs used pinned
Aravis 0.8.36 and externally provisioned eBUS 6.5.3.7155 with matching GenApi
3.4.1 headers. The scoped Pleora GenApi TSan exception was not changed.

See [PR #64](https://github.com/zackboll/ams-gra-gige-mel-provider/pull/64)
for the delivery-head matrix, optional GenTL fixture results and hosted runs.
