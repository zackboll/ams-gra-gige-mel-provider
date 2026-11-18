# Source-backed Image LOS metadata (GIGE-MEL-043)

The common provider implements working `LineOfSightEuler` and
`LineOfSightReport` callbacks **only for an explicitly qualified source**.
The SDK-free recording fixture qualifies the adapter mechanism, not real
sensor pointing. Default synthetic, Aravis, Pleora and GenTL devices have no
LOS source; both registrations remain `Return::NotSupported`, with neither
capability advertised. Quaternion and LOS3D behavior is unchanged.

## Pinned interpretation and local profile

The pinned IR MEL revision is `8d9224519f12b44e0b28815755c56a32a28d24a0`;
Common MEL is `f6908437d8fd2f7fb69896f9eb9cfd272d10c439`. No upstream
declarations or factory signatures change. `LineOfSightEuler` specifies
aperture Euler angles in radians and Euler rates in radians/second but has no
coordinate-frame field. `LineOfSightReport` specifies sensor-relative AzEl,
optional scan-speed/tolerance/platform attitude and radian image rotation.
Its validity bitfield has **no named masks** in the pinned closure.

This project therefore defines, rather than attributes to upstream, the
following narrow source profile. Sensor-local axes are right-handed forward,
right, down. Aperture orientation is intrinsic Z-Y-X; stored roll, pitch and
yaw correspond to X, Y and Z. Rates are Euler derivatives, not body p/q/r.
Azimuth increases toward sensor right; elevation increases toward up. Image
rotation is right-hand positive about aperture forward. This interpretation
requires independent source qualification and out-of-band consumer agreement:
MEL does not transmit its frame definition. Unknown declarations fail closed.
There is no implicit Euler/Report conversion, boresight, installation alignment
or navigation-to-pointing transform.

Time is a positive signed nanosecond count since an explicitly named source
origin, in `LosTimeDomain::source_relative` and
`LosEpoch::declared_source_origin`. A nonempty epoch meaning is mandatory.
It is neither UTC nor host reception time. The recording source names virtual
recording origin 043; its nonzero, distinguishable values are **test-generated**.
These timestamps are independent of image timestamps and are not associated
with frames. Other domains/epochs require a separately specified profile.

## Internal source interface

`include/gige_mel/line_of_sight.hpp` contains only project-owned doubles,
nanoseconds, enums, strings and optionals. `ILineOfSightSource` in
`include/gige_mel/backend.hpp` is an optional interface implemented alongside
`ICameraDevice`, reverse-direction to the unchanged `INavigationSink`.
It is a source-level matched-C++ interface, not a provider C ABI or stable
runtime plugin ABI. No MEL or vendor SDK type crosses this boundary.

`line_of_sight_support()` declares Euler and Report independently, plus frame,
time domain, epoch meaning, identity and provenance. These declarations must be
immutable for device lifetime, bounded to obtain, and established before Control
publishes immutable discovery. Invalid declarations disable both capabilities.
Attached Image capabilities and Control discovery use the same support facts.
Being able to construct a MEL payload is not a support declaration.

`open_line_of_sight(subscription_id)` creates a fresh owned stream, fixes a
nonzero source clock generation, and never replays pre-open observations.
It is bounded and called under `Runtime::operations`; neither source method
may call provider code or depend on work requiring that lock. `receive(50ms)`
waits for events rather than sampling; it owns its returned values and never
calls consumers. The source keeps at most one pending latest observation,
coalescing older pending observations under load. Cancellation is concurrent,
nonblocking and wakes receive promptly; the provider joins before destruction.
Backend implementers must document their finite open/receive/cancel bounds.
The recording implementation performs no I/O at support/open/cancel, uses a
single mutex and owned latest-value slot, and bounds its receive/delivery gate
by the supplied timeout. Fixture consumer gates have three-second watchdogs;
they are not source or production pointing measurements.

## Exact field mapping and validation

| Owned source fact | Upstream field | Admission |
| --- | --- | --- |
| `time` | Both `systemTime` | Exact positive nanoseconds; no cast through floating point |
| `euler.attitude.roll/pitch/yaw` | Euler `attitude` roll/pitch/yaw | All finite doubles, radians |
| `euler.rates.roll/pitch/yaw` | Euler `attitudeRates` roll/pitch/yaw | All finite doubles, radians/second |
| `report.angles.azimuth/elevation` | Report `pointingAngle.az/el` | Both present, finite, radians |
| `report.rates.azimuth/elevation` | Report `pointingAngleRates.az/el` | Both present, finite, radians/second |
| `report.image_rotation` | Report `imageRotation` | Present, finite double, radians |
| `report.validity_flags` | Report `validityFlagBitfield` | Only zero accepted |
| Optional `at_speed`, `in_tolerance`, `platform_attitude` | No supported validity claim | Must all be absent |

Report storage retains upstream false/zero defaults for absent optional values.
**Zero validity means absence of claims, not evidence of false atSpeed or
inTolerance.** Nonzero bits or any supplied optional status/platform value are
rejected rather than silently dropped or assigned invented masks. Complete
optional-field support remains blocked on independently established bit meanings.
Required fields cannot be replaced with zero placeholders when absent.

Every observation must match source identity, frame, epoch/domain, subscription
ID and the clock generation fixed at open. A generation change is rejected;
the source must explicitly restart to establish a new generation. Accepted
timestamps strictly increase separately per representation in that subscription.
Repeat/regression is rejected; malformed observations do not advance thresholds.
Either valid representation may publish even if the other is absent/invalid.
Finite doubles copy without narrowing, normalization, clamping or conversion.
The exact signed-nanosecond maximum is covered by the fixture oracle.

## Registration, publication and lifetime

For each advertised type, the first nonempty registration succeeds; empty gives
`BadPointer`, duplicate gives `Fail`. Registration before enable is supported.
Registration from any provider callback thread, detached channels or a closing
Control gives `Fail`. For an ordinary attached unsupported source, registration
always gives `NotSupported`, stores nothing and emits no placeholder. No initial
message or latest-value replay occurs at registration.

Image enable opens the subscription and starts one LOS worker only when support
exists. Publication cadence is event-driven, independent of image arrival.
One worker serializes LOS callbacks: Euler then Report if both are accepted in
one observation. There is no ordering promise relative to Image, C2 or Health
threads. A busy consumer causes latest-pending coalescing, not an unlimited queue.
Source failure stops LOS with a diagnostic, never fabricates pointing or images;
it does not change image acquisition/Health state. Disable/re-enable is explicit
recovery, not an automatic source reconnect.

Disable, active Standby, detach and Control destruction close publication and
cancel/join the worker. Fresh enable/attachment gets a never-reused subscription
ID. Already-admitted callbacks drain before external teardown returns; no new
callback occurs after completed teardown. The existing callback guard, failure
statistics and exception containment are reused. No consumer callback executes
while its thread holds operations, state or registration locks. Control/channel
pins exist across dispatch but not receive waits. Mutation from all provider
callback contexts is rejected. Defensive destruction on the worker itself uses
the existing Health-style cancelled self-detach pattern.

Shutdown has no unbounded backend update wait. As for existing Image/Health,
consumer callbacks must return: teardown must drain them and cannot forcibly
bound arbitrary consumer code. Sources cannot directly call MEL consumers.

## Qualification and non-goals

`provider_los_metadata` uses the explicitly selected, uninstalled recording
backend in `tests/support/recording_los_backend.hpp`. Its independent oracle
checks every upstream field with distinct nonzero doubles and exact times.
Tests exercise independent capabilities, malformed data, time continuity,
coalescing, exceptions, reentry, cancellation, concurrent images and teardown.
See the [design](tasks/GIGE-MEL-043-design.md) and
[executed validation](tasks/GIGE-MEL-043-validation.md).

This is a working callback adapter with source-backed **fixture qualification**,
not physical sensor LOS qualification, mounting/alignment accuracy, calibration,
navigation-to-pointing transformation, UTC synchronization or image timestamp/
frame synchronization. NavigationReport receipt and forwarding are unchanged;
neither proves aperture pointing. FrameHeader navigation/inertial state remains
empty. No camera SDK or sensor-specific mapping is added. This is not complete
AMS GRA MEL conformance.