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

## Limitations

- The camera's power-on configuration is unsupported. The documented
  temporary arrangement is needed (an operator-saved configuration was not
  attempted).
- Only full-frame 2592×1944 Mono8 at 2 Hz and 35 ms was exercised. ROI/apply
  writes, other rates, binning and 12-bit formats were not.
- The 100 Mb/s → 1 Gb/s link renegotiation was observed but not explained.
- Packet resends occurred at a 1500-byte MTU; no network tuning or loss
  qualification was done.
- No timing item is qualified; optics are virtual; no physical unplug,
  power-loss or heartbeat-timeout test was run.
