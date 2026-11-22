# Current capability and evidence status

This summary is maintained with the documented source tree; it does not
automatically track later revisions. See the
[Qualification Evidence](qualification-evidence.md) index for historical runs,
the [documentation index](index.md), the detailed
[implementation profile](implementation-profile.md), and the
[MEL requirements traceability inventory](mel-traceability.md).

**Experimental, partial MEL implementation; not full MEL 4.0 conformance or
production camera/timestamp qualification.** Implemented C++ channel classes and
unsupported stubs are not equivalent to functional, advertised capabilities.

## How to read the evidence

- **Implemented:** merged source provides the behavior under its stated admission
  and lifecycle contracts.
- **Evidence:** see the [classification and outcome definitions](qualification-evidence.md#how-to-read-the-index).
  SDK-free, native fixture, simulated network and physical observations are
  separate from production qualification.
- **Unsupported / planned:** unavailable or gated work. No capability may be
  inferred from a class declaration, design record or successful fixture run.

## Backend and camera-profile matrix

| Backend / profile | Implemented path and availability | Evidence and important limits |
| --- | --- | --- |
| Synthetic / default | SDK-free default; exported `getAPI_Manager("synthetic")`; deterministic Mono8, optional explicit Mono12, camera controls and registered-host-buffer MEL delivery | [SDK-free provider evidence](qualification-evidence.md#sdk-free-provider-and-profiles). Virtual integration-start clock and virtual optics only; no hardware/UTC claim. |
| Aravis / `mono_free_running_v1` | Default-OFF, internal/uninstalled Aravis **0.8.36** adapter: discovery, exact open, configuration/readback, bounded source acquisition, leases and terminal control-loss handling. Available in the internal catalog when compiled, not through exported MEL factory selection | [Fake and loopback](qualification-evidence.md#aravis-fake-and-simulated-gige) plus [physical Cognex observations](qualification-evidence.md#cognex-cic-5000r-physical-observations). Production `FrameTimestamp{}` is invalid; positive MEL pixels use a test-only virtual clock. |
| Pleora / `mono_free_running_v1` | Default-OFF, internal/uninstalled eBUS **Pv 6.5.3.7155** adapter with GenApi 3.4.1 runtime / 3.4.1.1 headers: exact MAC open, shared feature policy, bounded native reception and host-copy leases. Internal catalog when compiled; not an exported factory choice | [Native fixtures and watermarked evaluation](qualification-evidence.md#pleora-ebus). Default policy does not enable evaluation. SDK entitlement/production licensing unresolved; invalid timestamps preclude production MEL delivery. |
| Aravis or Pleora / `cognex_cic_5000r_v1` | Independently built matched-C++ module; exact COGNEX/CIC-5000R/V3.7-1 identity; Mono8 and conservative **1/2/4/5/8/10 Hz** admission | [Module guide](../camera_profiles/cognex_cic_5000r/README.md), [QE-COGNEX-MODULE](qualification-evidence.md#sdk-free-provider-and-profiles). Earlier physical observations informed policy; no physical qualification of the module, Mono12 or clocks. |
| GenTL / injected existing profiles | Default-OFF, internal/uninstalled **read-only** backend foundation using EMVA GenTL 1.6 and Aravis 0.8.36 GenApi. Enumerate, exact open/close, profile admission and readback; no `apply`, streaming or catalog registration | [QE-GENTL-BACKEND](qualification-evidence.md#gentl-producers-and-consumer) is controlled simulated-register fixture evidence only. Reuse of the Cognex module does not change its Aravis/Pleora compatibility metadata. |

The internal catalog's canonical default profile is `mono_free_running_v1`;
`aravis-mono8-free-running-v1` remains a historical diagnostic identifier.
Synthetic uses its own fixed policy, not an arbitrary loaded camera profile.
The alternate rate-limited example/test profile demonstrates independent policy
and lifetime behavior; it is not a supported physical-camera profile.

### Physical evidence is deliberately narrower than production support

The [Cognex record](cognex-cic-5000r.md) identifies serial 21874533, firmware
`106349-08;U;CIC-5000R;V3.7-1;0`, and full-frame 2592×1944 Mono8 observations.
Aravis 0.8.36 sustained 2, 5 and 10 Hz on the observed 1 Gb/s link; heavy-load
loss was also observed. The power-on rate-control-disabled configuration is
unsupported, and 14 Hz is not exactly representable under the integer-Hz
contract. Pleora's [R1 watermarked hardware evaluation](pleora-integration.md#r1-watermarked-hardware-qualification-2026-11-12)
has its own limited scope, not a paired benchmark or licensed production result.
Neither record qualifies integration-start timestamps, optics/calibration,
UTC/PTP, navigation, production reliability or full MEL conformance.

## MEL behavior summary

| Surface | Current implemented behavior | Boundary / evidence |
| --- | --- | --- |
| Loading and buffers | Exact upstream manager/control/buffer factories; independent sessions; copy-first registered host memory and explicit consumer release | Factory instance is exactly `synthetic`; unknown names return null. Held bytes must survive stop/close until release. [Architecture](architecture.md), [profile](implementation-profile.md). |
| Control discovery | Publishes only a complete snapshot. If discovery construction fails, the prior snapshot is unchanged and retry can use the already initialized device. | [SDK-free discovery regression](qualification-evidence.md#sdk-free-provider-and-profiles); no change in channel availability or hardware qualification. Do not race reads with initialization. |
| Image | Mono8 and explicitly selected unpacked little-endian Mono12; camera command/readback and bounded publication | Format admission is per device/profile, not a CameraCommand format switch. Aravis/Pleora require independently established byte order for Mono12; physical Cognex Mono12 is unqualified. [Mono12 contract](mono12-image-profile.md). |
| C2 | CameraCommand, complete ConfigSet v1, communications and bounded BIT/ModeCmd subsets | [Configuration](configuration.md), [BIT](software-bit.md), [ModeCmd subset](mode-command.md). Not full operational C2/tasking. |
| Health | Software MFA_Status, MFA_StatusDetailed, BIT_Status and SubsystemStatusResp | Observed provider-software state, not camera health or hardware BIT. [Health](health-status.md), [subsystem profile](subsystem-status.md). |
| BadPixelList | Known-empty metadata for the synthetic generator's explicitly declared defect-map evidence | No physical defect-map source, correction or calibration. [Profile](implementation-profile.md#synthetic-known-empty-badpixellist-gige-mel-016). |
| Navigation | Structural receipt validation/acknowledgement; optional typed backend sink forwarding when supplied; optional camera-profile handler behind a sink (internal, recording fixture only) | No production navigation transport, detector protocol, image association or navigation-quality claim. [Reception](navigation-reception.md), [forwarding](navigation-backend-forwarding.md). |
| Image LOS | Conditional Euler/Report support from a validated optional source, declared before discovery | Only an explicitly selected SDK-free recording fixture supplies qualified observations. Default synthetic and production adapters remain unsupported/unadvertised; no inferred pointing or physical time alignment. [LOS contract](image-los-metadata.md). |
| StackedImage | Conditional delivery of source-provided, pre-formed contiguous Mono8 volumes; explicit release and acquisition exclusion | Recording fixture only, not ordinary-frame concatenation. Default synthetic/production adapters unsupported/unadvertised; no sensor TSI/time qualification. [Stack contract](stacked-image-provider.md), [fixture validation](tasks/GIGE-MEL-044-validation.md). |
| Scheduling | Virtual stare admission, current abort and correlated admission acknowledgements | [Recording fixture only](task-scheduling-provider.md); no physical tasking or MEL completion reports. |

### Unsupported and planned work

- Track, Instrumentation and the alternate direct-channel
  factory/registry path are unimplemented.
- Calibration, protection, erase and tracking commands remain unsupported.
  Five unsupported C2 commands also issue correlated rejected CommandStatus;
  rejection reporting does not advertise support.
- Health DiscreteStatus and MFA_SecurityAuditRecord remain unsupported and
  unadvertised, with pinned-enum/event-source blockers recorded in the
  [Health gap classification](tasks/GIGE-MEL-041-design.md).
- Hardware integration-start time mapping, physical Mono12 qualification,
  licensed production Pleora use and GenTL streaming/selection remain gates,
  not delivered capabilities. Direct registered-storage acquisition and a stable
  cross-toolchain backend/profile ABI are not implemented.
- The [controlled GVCP](controlled-gvcp-transport.md) and
  [single-owner](gvcp-single-owner.md) work is loopback-only experimentation,
  not production SDK ownership integration or navigation transport. The internal
  [register/memory service](device-register-transport.md) adds no MEL capability
  and does not extend profile module v1.
- GenTL probe/supervisor/wire tooling adds no camera capability. In the merged
  [GENTL-003 results](tasks/GIGE-MEL-GENTL-003-validation.md), Balluff 3.7.2
  initializes then crashes during isolated interface update. Euresys 26.10 was
  unavailable at that time; the later [GENTL-003B record](tasks/GIGE-MEL-GENTL-003B-validation.md)
  reports successful native initialization but no exposed interface in the
  isolated loopback topology, so no device was opened. The
  [GENTL-003A wire harness](tasks/GIGE-MEL-GENTL-003A-validation.md) self-tests
  are not real-producer passes. Neither producer's read-only camera behavior is
  qualified by these records.

For method-by-method required/conditional/optional distinctions, use
[Requirements Traceability](mel-traceability.md). The
[roadmap](mel-surface-roadmap.md) preserves sequencing and milestone history.
Historical test conclusions remain in their original records.