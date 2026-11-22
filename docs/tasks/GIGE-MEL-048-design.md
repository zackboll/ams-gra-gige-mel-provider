# GIGE-MEL-048 — Backend layout and responsibility boundaries

## Baseline review

Reviewed `origin/main` at `5d3f58577864bd7c15c9200979f5ee46ef4a0e0d`.
The installed [backend contract](../../include/gige_mel/backend.hpp) separates
discovery and exact selection, open-device configuration and stream creation,
and acquisition/receive/stop and source leases. The internal
[typed feature service](../../src/device_features.hpp) exposes borrowed native
access during one serialized operation. The selected
[profile](../../src/profiles/camera_profile.hpp) admits the device and produces
per-device state that owns configuration and frame policy. The
[frame policy seam](../../src/profiles/frame_policy.hpp) carries native facts
and checked admission results, not SDK objects or ownership.

| Backend | Discovery and open device | Configuration and policy | Stream and source lease | Build boundary |
| --- | --- | --- | --- | --- |
| Synthetic | Fixed virtual identities and in-process state | Self-contained fixture options, capabilities and validation; no GenICam service | Generates virtual frames into a bounded pool. Leases pin source storage | SDK-free installed `gige_mel_backend` |
| Aravis | Exact discovered ID on a lazy native interface; reservation, `ArvDevice`, operation lock and control-loss subscription | Borrowed typed GenICam access, selected shared profile/state, native register access and read-only inspection | Native commands and their uncertain outcomes, fresh `ArvStream`/pool per run, timed pops, native observations and transfer-full buffer leases | Internal `gige_mel_aravis`, pinned Aravis/GLib; SDK Fake and GigE simulator in tests |
| Pleora | Exact discovered MAC, eBUS connection and event sink; device state owns the native stream/pipeline graph | Borrowed eBUS/GenApi feature access with fresh readback and selected shared profile/state | Production native source controls the device/pipeline; separate stream wrapper polls, validates, copies accepted bytes into host-owned lease storage and promptly requeues native buffers | Internal `gige_mel_pleora`, external eBUS and matching GenApi; controlled and native SDK tests |
| GenTL | Explicit producer and selected interface/device; owns producer/port handle graph | Private port-backed feature service and selected shared profile/state | Existing qualified-fixture path owns producer data stream/buffers and retires producer resources independently of retained host bytes | Optional internal GenTL backend and consumer; no new producer qualification |

Before extraction, synthetic's roles all lived in one SDK-free file.
Aravis combined all native control, feature, and acquisition roles in
`aravis_backend.cpp`, with capture and time-probe drivers already separate.
Pleora already split typed features and frame-stream processing from device
composition, with native pipeline coordination beside the device ownership.
GenTL organized port-backed GenApi separately from its backend. The internal
[selection catalog](../../src/selection/) constructs configured factories and
selects compatible policies explicitly; registration does not discover devices.
The optional SDK CMake modules own vendor dependencies, not the common provider.