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