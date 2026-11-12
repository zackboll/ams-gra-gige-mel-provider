# Partial required Image navigation reception (GIGE-MEL-015)

The receipt-only behavior below remains the default. The optional
[backend navigation forwarding extension](navigation-backend-forwarding.md)
adds submission before acceptance only for devices implementing a navigation
sink; its backend failure and threading rules supplement this contract.

This is **PARTIAL required navigation support**, not full navigation integration
or MEL conformance. The MEL consumer supplies `mel::NavigationReport`; the
provider validates structure, retains one report and acknowledges receipt.
It cannot establish the truth of a location, solution quality, sensor alignment,
mounting orientation, integration-start time, UTC/PTP synchronization or whether
a report corresponds to an image.

## Pinned interface

IR MEL `8d9224519f12b44e0b28815755c56a32a28d24a0` requires
`ImageChannel::send(mel::NavigationReport) -> RequestFor<NavigationReportResp>`
and `registerMetadataCallback(NavigationReportResp callback)`. Common MEL
`f6908437d8fd2f7fb69896f9eb9cfd272d10c439` defines the input, nested
`AttitudeRate`, `PositionVelocityCovariance`, `Euler` and `NorthEastDown`.
All declarations and enums remain unchanged.

Only Image advertises `ChannelMetadataCapabilityType::NavigationReportResp`.
C2 and Health do not. No new sensor, LOS, inertial or navigation-image
correlation capability is advertised.

## Complete structural validation

Validation uses the pinned getters and does not normalize, clamp or change input.

| Input | Accepted rule |
| --- | --- |
| `systemTime` | Positive `std::chrono::nanoseconds`, preserved without unit conversion; up to `nanoseconds::max()` |
| `state` | Exactly `Aligning`, `FreeInertial`, `GPS`, `Blended` |
| `latitude` | Finite, inclusive [-pi/2, pi/2] radians |
| `longitude` | Finite, inclusive [-pi, pi] radians |
| `altitude`, `altitudeMSL` | Finite |
| `attitude` | Finite roll, pitch, yaw |
| `attitudeRate.attitudeRate` | Finite roll, pitch, yaw |
| `attitudeRate.attitudeRateTime` | Nonnegative nanoseconds, no comparison with another clock |
| `speed`, `acceleration` | Finite north, east, down components |
| `wanderAngle`, `magneticHeading` | Finite |
| Covariance | All 18 fields finite; six diagonal variances nonnegative; cross terms may be signed |

The 18 covariance fields are `positionPositionPnPn`, `positionPositionPnPe`,
`positionPositionPnPd`, `positionPositionPePe`, `positionPositionPePd`,
`positionPositionPdPd`, `positionVelocityPnVn`, `positionVelocityPnVe`,
`positionVelocityPnVd`, `positionVelocityPeVe`, `positionVelocityPeVd`,
`positionVelocityPdVd`, `velocityVelocityVnVn`, `velocityVelocityVnVe`,
`velocityVelocityVnVd`, `velocityVelocityVeVe`, `velocityVelocityVeVd`,
`velocityVelocityVdVd`. The diagonal terms are PnPn, PePe, PdPd, VnVn, VeVe,
VdVd. These checks **do not prove positive semidefiniteness** or physical validity.

NaN and either infinity are rejected in every floating field. `NotSet`,
`MaxExclusive` and unknown numeric states are rejected. State names are consumer
labels: acknowledging Aligning does not promote it to a GPS fix; accepting
FreeInertial does not certify its accuracy. The provider does not qualify GPS
or Blended labels either.

## One latest accepted report per Image attachment

Private Image-owned storage contains at most one full report, never a queue,
history, manager-global cache or public query API. The first accepted timestamp
must be positive. Each subsequent accepted report must have **strictly greater**
`systemTime`. Equal/backward values fail with `InvalidState`. Any rejected input
leaves the retained report unchanged, including rejection during acknowledgement
preparation. Disable/enable does not reset this clock rule. Detach ends the
session; a newly attached Image starts fresh. An old detached handle cannot
submit to the new channel. No automatic epoch correction/reset exists.

The retained value is groundwork only for a later independently qualified
association feature. It is never consulted by frame production.

## Acknowledgement, errors and callback lifetime

On acceptance the provider constructs a non-null response and ready future,
then commits one latest report and dispatches the registered Image callback:

- `commandID = 0` and `reqId = 0`: **unassigned** in this profile. Input has
  neither identifier; no correlation ID is fabricated.
- `systemTime = input.systemTime`: an **exact echo**, not an SDK, host or camera
  acknowledgement timestamp. The echo establishes no shared clock domain.
- The callback's channel is the receiving Image. Its payload fields match the
  future response. The pointer is borrowed only for the callback duration;
  consumers may copy the value. The future owns an independent response object.
- No callback is required for successful receipt. Registration does not replay
  a retained/historical report. No C2 `CommandStatus` is emitted.

All normal rejected requests return ready typed MEL failures, never an
unfulfilled promise. Invalid time sign, coordinates, enum or nonfinite/negative
variance data yields `InvalidParameters` with a field-specific description.
Duplicate/backward time, callback reentry, detached or closing channel yields
`InvalidState`. An ordinary allocation failure during preparation maps to
`InsufficientLocalResources` without commit or callback (as with other APIs,
constructing even an error future requires available memory).

The first nonempty callback registration succeeds. Empty registration on a
usable channel returns `BadPointer`; duplicate, detached/closing and registration
from **any provider callback thread** return `Fail`. No replacement/unregister
API is invented. Callback exceptions are contained and counted by the existing
ChannelCore policy; the successful future remains available.

## Threading and detach

Reentry is rejected before operations, including cross-provider callbacks.
Submission pins Control and channel through synchronous dispatch. Existing
runtime operations serialize validation, monotonic admission and commit with
other submissions, Image lifecycle, CameraCommand and detach. Acknowledgement
and callback snapshot allocations precede the non-throwing report commit.
Callbacks are admitted before releasing operations, so external detach drains
even accepted callbacks not yet entered. No operation/state/callback mutex is
held while invoking user code. Concurrent submissions may dispatch on their
respective caller threads; callback completion order is not a history ordering
guarantee. Consumers must synchronize shared callback state.

Detach closes new admission and waits for admitted callbacks; after it returns
there are no stale callbacks. A blocking consumer can delay detach. Do not wait
inside a callback for work that depends on callback completion. Reentrant
submissions/lifecycle/registration are refused, not queued. Defensive Control
destruction follows existing provider policy. No navigation thread, SDK I/O,
Aravis device lock, configuration generation or Health/BIT mutation is added.
Submission works with acquisition disabled or enabled.

## Explicit non-association boundary and future gates

`FrameHeader::sensorNavState` and `sensorInertialState` remain empty, with all
existing frame defaults, timestamps, geometry, buffer ownership and pixels
unchanged. Received data is not transformed into LOS/pointing, interpolated,
extrapolated or associated with current or future frames. Production Aravis
timestamps remain invalid `FrameTimestamp{}` and Aravis-backed MEL publication
remains disabled by the unchanged timestamp admission policy.

Future association needs independently qualified clock domains/epochs,
discontinuity and uncertainty handling, camera integration-start provenance,
navigation quality/freshness criteria, coordinate conventions and calibrated
mounting/alignment/lever arms. It needs an explicit association/retention policy
and selected camera/navigation hardware evidence. None is implied by receipt.

## Public demonstration and regressions

`tests/public_navigation_demo.hpp` uses published headers only, shared by the
native loader and separately installed consumer. It discovers/attaches Image,
registers the response, sends deterministic **TEST DATA** labelled GPS, verifies
the ready future, exact echo, zero IDs and matching callback, then checks a stale
typed rejection without another callback. No buffers or acquisition are needed.
The feature-built loader's `--expect-no-navigation` mode checks the separately
built unchanged starting-main library returns `Unsupported`; `--navigation-only`
requires actual feature acceptance.

Six permanent provider scenarios cover every floating field's NaN/+Inf/-Inf,
six negative variances, time/enum/range boundaries, stale/no-mutation sessions,
callbacks and exceptions, preparation failure, concurrent reports and gated
lifecycle/configuration/detach/Control overlaps, independent managers and
frame/backend/Health/BIT noninterference. See [validation](validation.md).
