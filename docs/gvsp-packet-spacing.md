# GVSP inter-packet delay `GevSCPD` (GIGE-MEL-020)

GIGE-MEL-020 asked whether a bounded camera inter-packet delay (`GevSCPD`)
reduces UDP receive-buffer overflows and incomplete frames under heavy host
load. The [FIXED 4 MiB receive buffer](gvsp-receive-buffer.md) stayed in place
for every test.

> **Disposition: INCONCLUSIVE.** No tested delay (2, 4 or 6 µs) gave a
> repeatable improvement under comparable load. No leg reached 600/600 or
> 840/840 complete frames, and every leg had unrecoverable packets. The host
> load drifted during both sessions, and five of the six 14 Hz paired
> comparisons fell outside the agreed CPU band. **The adapter does not
> program `GevSCPD`, and its defaults are unchanged.** Nothing in this
> document is hardware qualification, and no production setting is
> recommended.

## Camera facts (Cognex CIC-5000R, firmware V3.7-1, 1 Gb/s link)

| Fact | Observation |
| --- | --- |
| Node | `GevSCPD` Integer → `MaskedIntReg` 0xd08 (GigE Vision bootstrap SCPD0), RW |
| Range | min 0. The max follows the link and the camera state: 12487 and 21839 were both read at 1 Gb/s, and 124930 at 100 Mb/s. Read it live every time |
| Increment / unit | No `<Inc>` and no `<Unit>` in the XML. Aravis reports `PROPERTY_NOT_DEFINED`, so the GenICam default increment of 1 applies |
| Locking | No `pIsLocked`, unlike `GevSCPSPacketSize`, which `TLParamsLocked` locks |
| `GevSCPDA` | Absent on this camera |
| Original value | 0 |
| Persistence | Volatile. It survives stream stop/restart. Pinned Aravis 0.8.36 never writes it on stream creation or destruction |

**Units, measured: 1 tick = 8 ns** (`GevTimestampTickFrequency` 125 MHz,
the same conversion as `arv_camera_gv_set_packet_delay()` in Aravis 0.8.36).
Readback of `GevSCBWA` matched 125e6 · 1541 / (1541 + `GevSCPD`) to within
0.01 %:

| `GevSCPD` ticks | Delay | `GevSCBWA` B/s | `ResultingFrameRateAbs` at a 14 Hz request |
| --- | --- | --- | --- |
| 0 | 0 | 125,000,000 | 14.000112 |
| 250 | 2 µs | 107,553,232 | 14.000112 |
| 500 | 4 µs | 94,380,196 | 14.000112 |
| 750 | 6 µs | 84,081,909 | 14.000112 |
| 800 | 6.4 µs | 82,286,180 | **13.952645** |
| 1000 | 8 µs | 75,809,917 | **12.854462** |

10 Hz holds at every candidate. At 14 Hz the camera lowers the rate above
750 ticks, so 750 ticks (6 µs) was the upper candidate. With `GevSCPD` 0 the
sensor limit is 14.592 Hz.

## Method

- **Owner authorization.** Temporary `GevSCPD` values of 0, 250, 500 and
  750 ticks; `net.core.rmem_max` 33554432 through an operator-started root
  guard limited to 90 minutes; synthetic load of at most 8 workers.
- **Camera arrangement.** Each session wrote only
  `AcquisitionFrameRateEnable`, `AcquisitionFrameRateAbs` and `GevSCPD`, each
  verified by readback. `GevSCPD` was written only while no stream existed. A
  trap restored all three on every exit path, then verified every recorded
  writable feature against the original snapshot.
- **Paths.** At 10 Hz the session used the **unchanged production adapter**
  (merged GIGE-MEL-019, FIXED 4 MiB). At 14 Hz it used a raw-SDK harness with
  the same FIXED 4 MiB request, because the production adapter rejects
  14.000112 Hz.
- **Receive buffer.** Every leg was gated on a kernel-reported `SO_RCVBUF` of
  8388608 (4 MiB granted). All 26 legs passed the gate.
- **Leg order.** 13 legs of 60 s per rate, with a `GevSCPD` 0 control
  between paced legs and a mirrored second half:
  `0 250 0 500 0 750 0 750 0 500 0 250 0`.
- **Pairing rule.** A paced leg counts as **better** only if it beats both
  neighbouring controls on `UdpRcvbufErrors` and on unrecoverable packets, and
  loses no more frames than the better neighbour. A bracket counts as
  **comparable** only if all three legs had host CPU within 41–69 % (the
  GIGE-MEL-019 band) and the paced leg was within 8 points of both controls.
- **Loss-free** means every frame complete, no frame-ID gap, zero
  unrecoverable packets and no timeout.
- **Load.** The 8 synthetic workers were started only after the baseline host
  CPU measured below 41 % (6.7 % before the 10 Hz session, 16.6 % before the
  14 Hz session). The owner's own builds and tools also ran during both
  sessions; they were neither started nor stopped.

## Results (2026-11-09)

Every leg ran its full 60 s, exited 0, stopped cleanly and read back the
intended `GevSCPD`. "Lost" is incomplete frames plus frame-ID gaps.
"Unrecov." is packets the SDK could not recover. No leg in either session
recovered a resent packet. NIC errors, NIC missed and softnet drops were 0 in
every leg.

### 10 Hz, production adapter

| Leg | SCPD | Complete | Lost | fps | UDP drops | Resend req. | Unrecov. | CPU % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 01 | 0 | 598/600 | 2 | 9.966 | 949 | 3,796 | 949 | 45.3 |
| 02 | 250 | 599/600 | 1 | 9.983 | 80 | 320 | 264 | 43.9 |
| 03 | 0 | 594/600 | 6 | 9.900 | 1,780 | 7,541 | 2,762 | 43.4 |
| 04 | 500 | 593/600 | 7 | 9.883 | 2,413 | 10,003 | 3,368 | 50.7 |
| 05 | 0 | 587/600 | 12 | 9.803 | 2,319 | 9,940 | 7,182 | 53.6 |
| 06 | 750 | 581/600 | 19 | 9.682 | 5,818 | 22,857 | 9,435 | 63.3 |
| 07 | 0 | 569/600 | 30 | 9.502 | 5,856 | 24,140 | 12,389 | 68.4 |
| 08 | 750 | 577/600 | 24 | 9.599 | 6,593 | 27,460 | 12,059 | 67.1 |
| 09 | 0 | 578/600 | 22 | 9.633 | 4,961 | 20,244 | 7,715 | 66.5 |
| 10 | 500 | 571/600 | 29 | 9.516 | 7,132 | 30,126 | 11,249 | 63.8 |
| 11 | 0 | 582/600 | 18 | 9.699 | 5,062 | 18,649 | 6,549 | **70.1** |
| 12 | 250 | 588/600 | 12 | 9.800 | 2,695 | 12,322 | 4,754 | 64.5 |
| 13 | 0 | 576/600 | 24 | 9.599 | 4,850 | 21,040 | 8,607 | 61.8 |

### 14 Hz (camera 14.000112 Hz), raw SDK

| Leg | SCPD | Complete | Lost | fps | UDP drops | Resend req. | Unrecov. | CPU % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 01 | 0 | 756/840 | 94 | 14.000 | 82,218 | 273,407 | 193,608 | **70.2** |
| 02 | 250 | 817/840 | 24 | 14.000 | 14,074 | 62,728 | 44,124 | 66.4 |
| 03 | 0 | 787/840 | 57 | 14.012 | 38,515 | 143,994 | 120,542 | 67.5 |
| 04 | 500 | 817/840 | 23 | 14.026 | 8,380 | 40,235 | 38,921 | 64.7 |
| 05 | 0 | 736/840 | 114 | 14.000 | 111,387 | 315,335 | 243,914 | 63.9 |
| 06 | 750 | 820/840 | 21 | 14.000 | 17,198 | 56,253 | 42,417 | 53.1 |
| 07 | 0 | 551/840 | 336 | 14.006 | 352,853 | 937,266 | 747,948 | **79.2** |
| 08 | 750 | 514/840 | 370 | 13.948 | 390,229 | 1,222,186 | 809,462 | **86.7** |
| 09 | 0 | 699/840 | 161 | 13.998 | 150,590 | 478,742 | 355,025 | **79.6** |
| 10 | 500 | 550/840 | 328 | 14.036 | 364,745 | 1,073,105 | 746,600 | **84.2** |
| 11 | 0 | 295/840 | 638 | 13.966 | 764,042 | 2,034,412 | 1,498,813 | **77.5** |
| 12 | 250 | 426/840 | 471 | 13.999 | 537,297 | 1,655,341 | 1,118,102 | **76.7** |
| 13 | 0 | 827/840 | 15 | 14.000 | 11,987 | 19,521 | 30,118 | 56.8 |

CPU values in **bold** are outside the 41–69 % band. During 14 Hz legs
07–12 the owner's own parallel builds ran on top of the synthetic load (see
[Deviation](#deviation-from-an-owner-condition)).

### Paired comparisons

| Rate | Bracket | SCPD | Result | Comparable? |
| --- | --- | --- | --- | --- |
| 10 Hz | 01-02-03 | 250 | better | yes |
| 10 Hz | 03-04-05 | 500 | not distinguishable | yes |
| 10 Hz | 05-06-07 | 750 | not distinguishable | no (paced leg 9.7 points from a control) |
| 10 Hz | 07-08-09 | 750 | not distinguishable | yes |
| 10 Hz | 09-10-11 | 500 | worse | no (control at 70.1 %) |
| 10 Hz | 11-12-13 | 250 | better | no (control at 70.1 %) |
| 14 Hz | 01-02-03 | 250 | better | no (control at 70.2 %) |
| 14 Hz | 03-04-05 | 500 | better | yes |
| 14 Hz | 05-06-07 | 750 | better | no (control at 79.2 %) |
| 14 Hz | 07-08-09 | 750 | worse | no (all above the band) |
| 14 Hz | 09-10-11 | 500 | not distinguishable | no (all above the band) |
| 14 Hz | 11-12-13 | 250 | not distinguishable | no (above the band) |

## Interpretation

- **No repeatable benefit was established.** Each rate had only one
  comparable bracket in which a paced leg was better: 250 ticks at 10 Hz and
  500 ticks at 14 Hz. Neither was repeated in a second comparable bracket. In
  comparable brackets 500 and 750 ticks did not help at 10 Hz. At 14 Hz,
  750 ticks was better once and worse once, and neither of those brackets was
  comparable. The smallest value with a *repeatable* improvement therefore
  does not exist in this data.
- **Suggestive only.** At 14 Hz with load inside or near the band
  (legs 01–06), all three paced legs (817–820/840) beat every control
  (736–787/840). That is about the GIGE-MEL-019 4 MiB level (820–828/840)
  and no better. Above the band (legs 07–12), pacing did not help.
- **Load drift dominates.** Within each session, losses tracked host CPU more
  closely than `GevSCPD`. The final 14 Hz control (leg 13, 56.8 % CPU) was
  the best 14 Hz leg of all.
- **Unrecovered resends.** No leg recovered a resent packet. That points at
  receiver-side scheduling rather than wire bursts, but the cause of the
  remaining loss is **not identified**.
- **Cost of pacing.** At 14 Hz a 6 µs delay cuts the wire rate to 84 MB/s,
  and above 750 ticks the achievable frame rate drops. At 10 Hz there is
  headroom, but no benefit was shown.

## Opt-in procedure (not applied by this project)

`GevSCPD` is a camera transport setting owned by the camera operator. The
adapter neither reads nor writes it, and it is not a MEL capability. To
repeat this experiment:

1. Record the original value and the live range (`GevSCPD`, its min and max,
   and `GevSCPDA` if present). Confirm the tick unit on the device before
   quoting a delay in time units, for example by comparing `GevSCBWA` with
   the expected wire rate. Do not assume 8 ns on another camera.
2. Write `GevSCPD` only while no stream is open. Check the value against the
   live min/max and the increment, then read it back with the GenICam cache
   disabled.
3. After writing, read `ResultingFrameRateAbs` and confirm the camera still
   meets the requested rate. On this camera at 14 Hz the limit is 750 ticks.
4. Restore the original value on every exit path and verify the readback. The
   value is volatile, so a camera power cycle also restores it. Never save a
   UserSet for this experiment.
5. Compare paced legs only against bracketing controls under comparable,
   measured load, with the receive buffer verified as granted.

## Deviation from an owner condition

The owner allowed synthetic load only while host CPU was below the 41–69 %
band. The runner checked the baseline only before starting the load for each
session. It logged a post-start check but did not act on it, and it did not
re-check before each leg.

- **14 Hz session.** The check 60 s after the 8 workers started read 82.9 %
  busy (load1 20.6), because the owner's own parallel builds were running too.
  The synthetic load nevertheless stayed on for the whole session
  (02:50–03:08Z).
- **Above-band legs.** 14 Hz leg 01 (70.2 %) and legs 07–12 (76.7–86.7 %)
  were above the band, and so was 10 Hz leg 11 (70.1 %).
- **Effect.** The bounded scope ended normally, and no camera or host fault
  resulted.
- **Handling.** Those legs are kept as data and excluded from the comparable
  comparisons.
- **For future runners.** Gate on the post-start check, re-check the band
  before every leg, and stop the synthetic load as soon as the band is
  exceeded.

## Restoration

- **Camera.** Five camera sessions in this task: a 10 Hz units check, a 14 Hz
  rate readback, the two heavy sessions and the physical MEL run. Each ended
  `RESTORE_VERIFIED`, with `GevSCPD` 0, `AcquisitionFrameRateEnable` false,
  `AcquisitionFrameRateAbs` 10, and every recorded writable feature equal to
  the original snapshot. An independent read after the heavy sessions
  confirmed the same state. All 12 camera snapshots reported `GevLinkSpeed`
  1000. No UserSet, packet size, `GevSCFTD`, IP or firmware setting was
  written.
- **Host.** The guard set `rmem_max` 33554432 at 02:31:15Z and restored
  212992 at 03:08:28Z (`RESTORED_OK`, verified by re-reading it), within its
  90-minute limit, which was never extended. `rmem_default` stayed 212992. No
  `/etc` file was written. The synthetic-load scope ended and no `cpuload`
  process remained.

## Limitations

- One camera, one host, one switch port and Aravis 0.8.36. Legs were 60 s,
  with one pass per value in each half.
- The 14 Hz path is raw SDK only; 14 Hz remains unsupported by the production
  adapter.
- Not tested: delays other than 2, 4 and 6 µs, `GevSCFTD`, jumbo frames, CPU
  or IRQ affinity.
- The opt-in physical MEL test ran at the original `GevSCPD` 0 and the
  default `rmem_max`, because no delay was shown to help.
- Production timestamps remain invalid.

