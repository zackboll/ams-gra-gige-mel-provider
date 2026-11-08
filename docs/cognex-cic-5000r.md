# Physical camera record: Cognex CIC-5000R (GIGE-MEL-018)

This is the first **physical** GigE Vision camera used with the optional Aravis
adapter. It records what was actually observed, what the adapter needed to
change, and what remains **unqualified**. Raw evidence (GenICam XML, PGM,
JSONL, collector bundles, session logs) is kept **outside Git** by the
operator and is not committed.

> **Status.** Camera control, Mono8 capture and test-only MEL delivery work on
> this camera under a documented **temporary, volatile** arrangement. The
> camera's own power-on configuration does **not** satisfy the profile.
> Production Aravis timestamps remain **invalid**. None of the 13
> [camera-timing checklist](aravis-integration.md#camera-timing-qualification-checklist)
> items is qualified.
>
> **1 Gb/s requalification (GIGE-MEL-018R2).** On a stable 1 Gb/s link the
> production adapter sustained 2, 5 and 10 Hz full-frame Mono8 with every frame
> complete. The camera cannot be set to exactly **14 Hz**: it quantizes the
> period to whole microseconds and reads back 14.000112 Hz, which the
> integer-Hz profile correctly rejects. 14 Hz is therefore **unsupported** by
> the production adapter; it was characterized only through a raw-SDK probe.
> See [1 Gb/s requalification](#1-gbs-requalification-gige-mel-018r2).

## Identity (observed 2026-11-08)

| Item | Observed value | Source |
| --- | --- | --- |
| Manufacturer / model | `COGNEX` / `CIC-5000R` | GVCP discovery ACK; `DeviceVendorName`, `DeviceModelName` |
| Serial number | `21874533` | GVCP discovery ACK serial field; `DeviceID` (StringReg 0xd8) |
| `DeviceSerialNumber` | Integer SwissKnife node; reads back `0` | GenICam XML |
| Firmware | `106349-08;U;CIC-5000R;V3.7-1;0` (`DeviceVersion` `106349-08`) | `DeviceFirmwareVersion` |
| MAC / IPv4 | `00:30:53:1c:9a:65` / 192.168.24.101/24, no gateway (persistent IP) | GVCP discovery ACK |
| GigE Vision | 1.1 | GVCP discovery ACK |
| GenICam XML | 1,108,591 bytes, SHA-256 `15bee3f741af8ee2582a88b73fe62515971eeb1db9f7e7f73e6ea64192f8d73f` ("Cognex generic GigEVision camera interface") | `arv_device_get_genicam_xml`; every `time-probe` header |
| Aravis device ID | `COGNEX-CIC-5000R-21874533` | `gige_mel_aravis_diagnostic --discovery-interface enp5s0 list` |
| Aravis | 0.8.36, compiled and loaded from the provisioned prefix | diagnostic banner, `ldd` |

The ordering suffix `-14-G` of the requested part number is **not** reported by
the camera and is therefore not confirmed.

## Host and network

The host uses its existing wired NIC `enp5s0` (192.168.9.10/24, default route
unchanged). The camera kept its persistent address on 192.168.24.0/24, so the
operator added a **temporary, non-persistent** secondary address
192.168.24.10/24 on `enp5s0`; `ip route get 192.168.24.101` then resolves
directly on `enp5s0` with source 192.168.24.10. Without it, the broadcast
discovery reply arrives but unicast GVCP is routed to the default gateway and
the camera cannot be opened. No host firewall was active. The camera's IP,
firmware, UserSets and persistent configuration were not changed.

**Link speed.** At the start of testing the camera reported `GevLinkSpeed`
100 (Mb/s) and `GevSCBWA` 12,500,000 B/s, with `ResultingFrameRateAbs`
2.1278 Hz for 2592×1944 Mono8 (5,038,848 bytes): 2.1278 × 5,038,848 × 8 ≈
85.8 Mb/s, so the full-frame rate was limited by a 100 Mb/s camera link.
Between two test sessions, with no camera write and no host change (the host
link stayed at 10 Gb/s), the camera started reporting `GevLinkSpeed` 1000 and
`GevSCBWA` 125,000,000 B/s, and `ResultingFrameRateAbs` rose to 14.59 Hz with
the original settings. The cause of the renegotiation (switch port, PoE,
cabling) is **not established**; the switch was not inspected.
`DeviceMaxThroughput` (100) and `GevSCBWR` (10) did not change; no
bandwidth-limiting feature was written.

## Features (read-only inspection)

| Area | Observation |
| --- | --- |
| Sensor | `SensorWidth` 2592, `SensorHeight` 1944 |
| ROI | `Width` [64, 2592] inc 1, `Height` [64, 1944] inc 1, `OffsetX`/`OffsetY` [0, 0] at full size; all RW |
| PixelFormat | current `Mono8`; available `Mono8, Mono12, Mono12Packed, YUV422Packed, YUV422_YUYV_Packed` (`PixelSize` Bpp8, `PixelDynamicRangeMax` 255) |
| Frame rate | **`AcquisitionFrameRateAbs`** float `<Unit>Hz</Unit>` [0.0149, 1,000,000]; `AcquisitionFrameRate` **absent**; `AcquisitionFrameRateEnable` Boolean (power-on **false**); `ResultingFrameRateAbs` RO Hz |
| Exposure | `ExposureTimeAbs` float `<Unit>us</Unit>` [35, 999,985]; `ExposureTimeRaw` integer inc 35; `ExposureMode` `Timed` only; `ExposureAuto` `Off` |
| Trigger / acquisition | `TriggerSelector` `FrameStart` (also `AcquisitionStart`), `TriggerMode` `Off`, `TriggerSource` `Line1`; `AcquisitionMode` `Continuous` (also `SingleFrame`) |
| Other | `GainAuto` Off, `GainRaw` 0, `BlackLevelRaw` 8, binning 1×1, `ReverseX` false, `ChunkModeActive` false, `SequenceEnable` false, `TestImageSelector` Off |
| Transport / time | `GevSCPSPacketSize` 1500, `GevSCPD` 0, `GevHeartbeatTimeout` 3000 ms, `GevTimestampTickFrequency` 125,000,000 Hz; no `GevIEEE1588`/`PtpEnable` feature |

Mono8 is the camera's genuine current output format (selected in the camera
before testing). The adapter neither assumed nor selected it; 12-bit formats
remain unsupported by the profile.

## Profile compatibility and the narrow adapter changes

Two observed incompatibilities were corrected in the adapter. Profile
semantics did not change:

1. **Frame-rate feature name.** The profile required `AcquisitionFrameRate`.
   The adapter now selects the first **implemented** of
   `AcquisitionFrameRate`, `AcquisitionFrameRateAbs`, with exactly the
   existing exposure-selection rule: a failed implementation query is an
   error and never falls through. Unit, bounds, increments, integer-Hz
   representability and write order are unchanged and use the selected name.
   A different selection at apply/start than at open is the existing "close
   and reopen" rejection.
2. **Integer identity node.** `DeviceSerialNumber` is a GenICam Integer node
   on this camera, which made read-only inspection report a type error.
   Identity reads now render an Integer node as its exact decimal readback
   labelled `(GenICam Integer node)`. Inspection also lists
   `DeviceFirmwareVersion` and `DeviceManufacturerInfo`. The value `0` is
   reported as read; the serial evidence is `DeviceID` and the GVCP ACK.

**Still unsupported: the power-on state.** With `AcquisitionFrameRateEnable`
= false, the frame-rate feature reads 10 Hz while the camera actually runs at
`ResultingFrameRateAbs` (2.128 Hz at 100 Mb/s), so the configured rate is not
a truthful readback. The adapter still never writes `AcquisitionFrameRateEnable`
(unchanged policy), and inspection reports `PROBLEM: unsupported profile:
AcquisitionFrameRateEnable is false`.

## Temporary arrangement used for hardware tests

With owner approval, each hardware session wrote **exactly two volatile
features** with a separate helper kept outside the repository, verified both
by readback, ran the tests, and restored them in a trap that runs on every
exit path:

| Feature | Original | During tests |
| --- | --- | --- |
| `AcquisitionFrameRateEnable` | false | true |
| `AcquisitionFrameRateAbs` | 10 Hz | 2 Hz |

No UserSet was saved or loaded, and no IP, firmware, transport or persistent
feature was written. After each of the three sessions, all 37 recorded
features were read back and every **writable** one (rate, exposure, ROI,
binning, format, trigger, gain, black level, chunk, sequence, transport,
heartbeat, persistent IP, UserSet selectors, user ID) equalled the original
snapshot. The only differences were read-only values that follow the
negotiated link (`GevLinkSpeed`, `GevSCBWA`, the `GevSCPD` maximum,
`ResultingFrameRateAbs`). A camera power cycle also discards the arrangement.

## Physical capture and stream evidence

| Check | Result |
| --- | --- |
| `inspect` | profile supported; envelope sensor 2592×1944, increments 1, rate 1–1,000,000 Hz, minimum exposure 35,000 ns; configuration 2592×1944@(0,0), 2 Hz, 35,000,000 ns |
| `capture` → `/tmp/cognex-capture.pgm` | native frame ID 1, stream generation 1, source row stride 2592; delivered 1, incomplete 0, timeouts 0, pool starvation 0 |
| PGM validation | `P5`, 2592×1944, maxval 255; 17-byte header + 5,038,848 pixel bytes = 5,038,865 bytes; pixel values 0–6 (mean 1.16; dark scene), rows differ |
| `time-probe` 8 and 16 frames | 8/8 and 16/16, 0 incomplete, 0 timeouts, 0 pool starvation |
| `aravis_evidence.py collect --acquire --frames 8`, then `verify` | `EVIDENCE_COLLECTED_AND_VERIFIED (NOT hardware/timestamp qualification)` |
| SDK GVSP statistics (16 frames, 1500-byte packets, 1 Gb/s link) | 16/16 complete; 0 failures, underruns, timeouts, aborted or missing frames; 0 missing, error or duplicated packets; **579 packets recovered by resend** out of 55,104 received |
| AcquisitionStop / release | every run stopped (`stop=ok`), all SDK leases returned, and a fresh production backend reopened and closed the device |

The resend count is real packet loss at a 1500-byte MTU, recovered by the
SDK's resend mechanism. Jumbo frames, `GevSCPD`, socket buffers and switch
behavior were not tuned or qualified. The very dark image proves only that the
pixel path is genuine; scene, focus and optics were not assessed.

## Test-only MEL integration (opt-in)

`tests/aravis_physical_camera.cpp` (target `aravis_physical_camera_tests`,
CTest `aravis_physical_camera`, label `hardware`) builds only with
`-DGIGE_MEL_BUILD_ARAVIS=ON -DGIGE_MEL_ARAVIS_HARDWARE_TESTS=ON
-DGIGE_MEL_HARDWARE_DEVICE_ID=<id> [-DGIGE_MEL_HARDWARE_INTERFACE=<nic>]`.
The hardware option defaults OFF and CI never sets it. The test writes no
camera feature; the camera must already satisfy the profile. It uses
`aravis::make_backend()`, the existing **test-only** virtual
integration-start clock and `gige_mel::make_api_manager()`:

| Scenario | Observed on the CIC-5000R (2 Hz, 35 ms) |
| --- | --- |
| G: production gate, no virtual clock | 3 physical frames received, 3 rejected (invalid `FrameTimestamp`), 0 published, destination untouched |
| V: Image + C2 attached, two registered host buffers | genuine 2592×1944 Mono8 pixels published; every destination byte equals the SDK lease bytes row by row; header geometry, ROI, integration time 35,000,000 ns and frame ID match; 5,038,848/5,038,848 destination bytes written |
| Backpressure | both buffers held; 2 further physical frames dropped; held bytes unchanged |
| S: `ModeCmd` Standby/Unused (command 1801) | ready `Unused`, `Accepted`; all source leases returned; no new SDK frame for three frame periods; `enable()` refused while buffers are held; held bytes unchanged |
| Release and restart | explicit `release()`; `enable()` succeeds; stream generation 2 → 3; frames published; `disable()` stops acquisition; 0 stream or callback failures |
| R: clean release | a fresh production backend reopens and closes the exact device |

`systemTime` in these callbacks is a declared **TEST MODEL** value, not camera
time, exposure start, PTP or UTC.

## Timestamp observations (not qualification)

At the device-reported 125 MHz tick frequency, the SDK-converted GVSP leader
timestamps advanced by 500,000,304–500,000,312 ns per frame at 2 Hz. Host
receive wall-clock intervals varied over a range of 93–139 µs. The camera
counter restarted (from ≈31,553 s to ≈218 s) between two sessions, coinciding
with the observed link renegotiation. That indicates a camera-local,
reset-dependent epoch. The apparent camera-to-host drift was −4.8 to +8.9 ppm
over a few seconds.

None of this establishes the timestamp event (integration start versus frame
start, end or readout), the epoch, synchronization (no PTP feature is exposed),
continuity across resets, or the path the stream actually used
(`actual_stream_timestamp_path` stays `"unverified"`). Production
`FrameDescriptor` timestamps remain `FrameTimestamp{}`, and the provider
continues to reject them.

## 1 Gb/s requalification (GIGE-MEL-018R2)

Performed 2026-11-08 after the camera was moved to a Gigabit switch port.

**Link and identity.** Before and after every session the camera reported
`GevLinkSpeed` 1000 and `GevSCBWA` 125,000,000 B/s, with `GevSCPSPacketSize`
1500, `GevSCPD` 0, `GevSCPSDoNotFragment` true, and `ResultingFrameRateAbs`
14.592 Hz at the original settings. Identity, firmware and Aravis 0.8.36 were
as above, and the original writable state matched the GIGE-MEL-018 snapshot.
The host link (`atlantic` driver, 10 Gb/s full duplex, MTU 1500, RX ring
2048/8184) and routing were unchanged. No MTU, switch, NetworkManager, route,
sysctl or camera transport setting was changed. The switch itself was not
accessible.

**Rate representability.** Only `AcquisitionFrameRateEnable` and
`AcquisitionFrameRateAbs` were written (35 ms exposure kept). The camera stores
the frame period in whole microseconds (`1e9 / (TimeBase 1000 ns × Hz)`,
truncated), so only rates with an integral period read back exactly:

| Requested | Exact readback | Classification |
| --- | --- | --- |
| 2, 5, 10, 16 Hz | 2, 5, 10, 16 | Representable; 16 Hz exceeds the link capacity (`ResultingFrameRateAbs` stays 14.592) |
| 12 Hz | 12.000048000192001 | Unsupported by the integer-Hz profile |
| 13 Hz | 13.000013000013 | Unsupported |
| **14 Hz** | **14.000112000896006** | **Unsupported**: `configuration()` reports "AcquisitionFrameRateAbs reads back 14.000112000896006 Hz, which is not representable as a positive integer Hz; the value was not rounded", and `Control::init` fails. Nothing was clamped or rounded. |

Integer rates up to 16 Hz with an exact period are 1, 2, 4, 5, 8, 10 and 16
Hz. **10 Hz is the highest integer rate the production adapter can use** on
this camera at full resolution.

**Sustained capture through the production adapter.** Each run used the
production GigE adapter (`make_backend_for_interface` with the GigE Vision
source, `create_stream`, `receive_with_native_timestamps`, `stop`, `close`)
from a harness kept outside Git. That harness attached one observation-only
hook to read the SDK's own GVSP counters, and it reopened the device after
release.

| Rate | Duration | Frames | Delivered fps | Throughput | Incomplete / ID gaps / timeouts | Host interarrival p50 / p99 / max (ms) | Raw camera timestamp increment (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2 Hz | 20 s | 41 | 2.0025 | 9.62 MiB/s, 80.7 Mb/s | 0 / 0 / 0 | 500.01 / 544.19 / 544.19 | 500.0000 every frame |
| 5 Hz | 30 s | 151 | 4.9999 | 24.03 MiB/s, 201.6 Mb/s | 0 / 0 / 0 | 200.00 / 242.34 / 243.42 | 200.0000 every frame |
| 10 Hz | 60 s | 600 | 9.9998 | 48.05 MiB/s, 403.1 Mb/s | 0 / 0 / 0 | 100.00 / 132.54 / 227.67 | 100.0000 every frame |
| 10 Hz (repeat) | 60 s | 600 | 10.000 | 48.05 MiB/s, 403.1 Mb/s | 0 / 0 / 0 | 100.00 / 100.05 / 121.90 | 100.0000 every frame |

Every run had 0 pool starvation, 0 SDK failures, underruns, timeouts, aborted
or missing frames, and 0 missing, duplicated or error packets. Native frame IDs
were contiguous from 1 with no wrap. No terminal error occurred. Every run
stopped (5.5–94.6 ms) and closed (8.8–17.3 ms) cleanly; the native stream was
finalized, and the device reopened. Harness CPU use was 3.8 / 8.0 / 15.6 / 12.1
% of one core, and peak RSS was about 41 MiB. Sampled content hashes changed
between samples (sensor noise), and the descriptor timestamp was invalid on
every frame.

**14 Hz characterization (raw SDK, not the production adapter).** Because the
production adapter cannot represent 14.000112 Hz, a separate raw-SDK probe
(the same 4-buffer pool and default SDK stream properties, packet resend
`ALWAYS`) streamed 60 s at that rate twice:

| Run | Buffers | Complete | Complete fps | Throughput | SDK timeouts | Missing packets | Host interarrival p99 / max (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 (host load avg ≈ 21) | 840 | 823 | 13.716 | 65.9 MiB/s, 552.9 Mb/s | **17 frames** | 31,065 | 137.08 / 238.56 |
| 2 (host load avg 7.3–9.0) | 840 | 840 | 13.9998 | 67.3 MiB/s, 564.3 Mb/s | 0 | 0 | 71.46 / 92.48 |

In run 1 the 17 failed frames were SDK packet-timeout frames, clustered in
bursts (IDs 479–489, 502, 539–540, 567–573, 743, 745). For 2,082 packets the
camera reported the data was no longer available (`n_error_packets`
2,082 = `n_resend_disabled` 2,082), so resend could not recover them. Run 2 had
no failed frame. Frame IDs were contiguous in both runs. So 14 Hz is
physically achievable on this link, but it is **not reliable under host load**
with the pinned SDK defaults, and it is not usable through the production
adapter.

**Packet loss and resends.**

| Run | Received packets | Resend requests | Resent packets | Resent / received | Kernel `UdpRcvbufErrors` | Host load |
| --- | --- | --- | --- | --- | --- | --- |
| GIGE-MEL-018 baseline, 2 Hz, 16 frames | 55,104 | 579 | 579 | 1.051 % | not measured | not measured |
| 2 Hz, 20 s | 141,204 | 2,163 | 2,163 | 1.532 % | 2,163 | ≈21–25 |
| 5 Hz, 30 s | 520,044 | 6,457 | 6,431 | 1.237 % | 6,457 | ≈21–25 |
| 10 Hz, 60 s | 2,066,400 | 42,590 | 40,540 | 1.962 % | 43,117 | ≈21–25 |
| 10 Hz, 60 s, repeat | 2,066,400 | 4,265 | 4,171 | 0.202 % | 4,265 | 7.2–9.3 |
| 14 Hz raw run 1, 60 s | 2,884,716 | 180,920 | 168,721 | 5.849 % | 189,164 | ≈21 |
| 14 Hz raw run 2, 60 s | 2,892,960 | 3,464 | 3,428 | 0.118 % | 3,464 | 7.3–9.0 |

Classification:

- **Where packets were lost: the host UDP socket receive buffer.** In every run
  the kernel's `UdpRcvbufErrors` delta matched the SDK's resend requests (to
  within the few packets that the camera answered with "no longer available").
  NIC counters showed no hardware loss: `InDroppedDma` 0, `InErrors` 0,
  `softnet_dropped` 0, and `rx_dropped` grew by only 11–33 per run, which is
  unrelated multicast or broadcast traffic. No duplicated or out-of-range
  packets were seen (`n_duplicated_packets` 0, `n_ignored_packets` 0).
  Out-of-order arrival without loss was not separately measurable with the
  pinned counters.
- **Recovery:** at 2–10 Hz every lost packet was recovered by resend or by
  late arrival, so no frame was incomplete. Only the 14 Hz run under heavy host
  load exceeded what resend could recover.
- **Cause:** the pinned stream defaults to socket-buffer `FIXED` with size 0, so
  `SO_RCVBUF` is never set. The stream socket therefore used
  `net.core.rmem_default`, 212,992 bytes (`ss`: `rb212992`), which holds about
  0.04 of a 5,038,848-byte frame. The camera sends each frame as a 1 Gb/s burst
  with `GevSCPD` 0. Loss **scales with host scheduling load, not mainly with
  frame rate**: at the same rate it fell about 10× (10 Hz: 1.96 % → 0.20 %; 14
  Hz: 5.85 % → 0.12 %) when the unrelated build load on this 32-core host
  dropped. Shared LAN traffic was not a measurable factor: the camera has its
  own switch port and subnet, and non-camera receive traffic was small.
- **Process-local socket-buffer experiment (raw SDK).** Setting
  `socket-buffer` `AUTO` for only that process requests about one payload, but
  without privilege the kernel caps it at `rmem_max` 212,992 (`ss` showed
  `rb425984`, the kernel's doubled value). That is no real increase, and it
  showed no consistent benefit: at 10 Hz A/B/A gave 0.022 % / 0.039 % / 0.282
  %, and at 14 Hz A/B gave 0.154 % / 1.748 %. All legs were complete (300/300
  and 420/420). The spread between runs exceeds any effect of the setting.

**Proposed follow-up (not applied).** This is a measured and reversible
experiment that needs owner approval, because it changes host-wide settings:

1. Run a baseline at the current settings: three 60 s 14 Hz raw-SDK runs and
   three 10 Hz production runs with a recorded host load.
2. Temporarily set `sudo sysctl -w net.core.rmem_max=33554432` (current
   212,992). Keep `rmem_default` and the camera transport unchanged.
3. In the adapter, set the stream's `socket-buffer` to `AUTO` (requests
   PayloadSize + 1 KiB ≈ 5.04 MB). This would be a production code change
   proposed for review, not made here.
4. Repeat the same runs and compare `UdpRcvbufErrors`, resends and incomplete
   frames.
5. Restore with `sudo sysctl -w net.core.rmem_max=212992`.

Only if loss remains, consider `GevSCPD` packet spacing or jumbo frames as
separate experiments. Neither was changed here.

**Hardware MEL (opt-in test, production adapter and test-only clock).** The
test now checks 10 restarted frames, not 1. Every published frame is fully
validated and source frame IDs must strictly increase within the new
generation.

| Rate | Result |
| --- | --- |
| 2 Hz | 3/3 PASS: gate 3 frames rejected and none published; 2 held, 2 dropped while held; Standby `Accepted` (1801); stream generation 2 → 3 with 10 checked frames (IDs 1..10); 5,038,848/5,038,848 destination bytes written; 14.7 s |
| 10 Hz | 3/3 PASS: same checks at 10 Hz; 7.0 s |
| 14 Hz | **Not executable**: `Control::init` fails because the configuration readback of 14.000112 Hz is not representable as integer Hz, which is the expected fail-closed behaviour |

Standby stopped acquisition (no new SDK frame for at least three frame
periods), the worker joined, and held bytes were unchanged until explicit
release. The restart produced complete frames, and no source lease, stream or
callback failure occurred. `systemTime` is a TEST MODEL value. No
integration-start time is claimed.

**Settings restoration.** There were 21 R2 hardware sessions: 18 kept,
including 7 readback-only rate probes, plus 3 superseded MEL attempts whose
test binary never ran because `/usr/bin/time` was missing. All 45 camera
snapshots taken during them reported `GevLinkSpeed` 1000. Each session wrote
only the two rate features, then restored
`AcquisitionFrameRateAbs` 10 and `AcquisitionFrameRateEnable` false. Each read
all 37 recorded features back, and every writable one equalled the original
snapshot.

One operator error is recorded. After a shell timeout interrupted a session,
a verification read raced with the next session's arrangement and saw
`AcquisitionFrameRateEnable` true. A later re-read confirmed the original
state. No UserSet was saved, and no gain, transport, IP or firmware setting
was written.

## Limitations

- The camera's power-on configuration is unsupported. The documented
  temporary arrangement is needed (an operator-saved configuration was not
  attempted).
- Full-frame 2592×1944 Mono8 at 35 ms was exercised at 2, 5 and 10 Hz
  through the production adapter (and at 14.000112 Hz only through a raw-SDK
  probe). ROI/apply writes, binning, other exposures and 12-bit formats were
  not.
- **14 Hz is unsupported by the production adapter**: the camera's
  microsecond-period quantization gives a non-integer readback. The highest
  usable integer rate is 10 Hz. The integer-Hz profile contract was not
  changed.
- The original 100 Mb/s link cause is unexplained. The 1 Gb/s link was stable
  throughout R2.
- Host socket receive-buffer drops (`rmem_default`/`rmem_max` 212,992 bytes)
  cause resends at a 1500-byte MTU. They are recovered at 2–10 Hz, but under
  heavy host load at 14 Hz they caused 17/840 failed frames. Tuning is only
  proposed, not applied.
- No timing item is qualified; optics are virtual; no physical unplug,
  power-loss or heartbeat-timeout test was run.
