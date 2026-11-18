# GIGE-MEL-043 pre-implementation design

Base: `9ded1b496c7e3b3811f78d57d3681c817256c555`, tree
`64fc6c137d04ef81e14342f8e7009b447acc9ccd`.

## Upstream boundary

Reviewed the pinned IR MEL `8d9224519f12b44e0b28815755c56a32a28d24a0`
ImageChannel, LineOfSightReport/Euler/Quaternion, ChannelCapability,
ChannelMetadataCapabilityType, FrameHeader and Common MEL
`f6908437d8fd2f7fb69896f9eb9cfd272d10c439` NavigationReport/Euler declarations.
Euler supplies nanosecond validity time, aperture Euler angles (radians), and
Euler derivatives (radians/second), but no frame or epoch definition. Report
specifies sensor-relative AzEl and radian imageRotation; optional atSpeed,
inTolerance and platformAttitude have an unnamed validity bitfield. No masks
or Euler-to-Report conversion can be established from these declarations.
Quaternion and LOS3D remain unsupported. No upstream files will change.

## Selected local interpretation (not an upstream convention)

Only an explicitly declared `sensor_frd_intrinsic_zyx` source contract is
accepted: sensor-local right-handed X forward, Y right, Z down; aperture
orientation intrinsic Z-Y-X, stored roll(X), pitch(Y), yaw(Z), and their Euler
derivatives (not body angular velocity). Sensor-relative azimuth is positive
toward right, elevation toward up; image rotation is right-hand positive about
aperture forward. Sources must independently establish this relationship to
the aperture. There is **no transformation** and no inference from navigation.

Time is positive signed nanoseconds in a source-defined relative domain, since
an explicitly named source origin. It is not host receive time, UTC, PTP or an
image clock. Source identity, provenance and epoch meaning must be nonempty.
The fixture identifies a virtual recording origin and test-generated values.
Consumers must agree out-of-band to this local profile: MEL has no field to
carry these declarations. Unknown frames/domains/epochs fail closed.

## Ownership and execution

Use an optional device-side source, reverse-direction analogue of
INavigationSink. Query immutable support before Control discovery is built.
Independently declare Report and Euler; require complete qualified declarations
before either is advertised. Open a fresh owned event stream on successful
Image enable, tagged with a manager-lifetime increasing subscription ID.
Source calls under operations must be bounded and cannot call provider code.
Receive waits for events (50 ms maximum wait), not periodic sampling. Source
storage is owned values, bounded to one pending latest observation (coalescing).
No MEL consumer is called by the source.

One LOS worker exists only for a supported source. It never acquires operations.
It uses weak Control/channel references across receive waits and pins both only
for publication. Reuse ChannelCore callback context, admission, exception and
detach-drain machinery. No callbacks under any runtime/registration lock.
Stop closes admission, requests cancellation and joins; defensive destruction
on the worker itself detaches that already-cancelled worker, as Health does.
Consumer callbacks must return, as with existing Image/Health: no mechanism can
bound an arbitrary blocking consumer. Backend receive/cancel must be bounded.

Image disable, Standby, detach and Control destruction retire the subscription.
Restart opens a fresh stream with no replay; old subscription IDs are rejected.
Within a subscription, fix the source clock generation at open and require
strictly increasing accepted timestamps separately per representation. Reject
generation changes until explicit re-enable; never guess continuity. Invalid
observations do not advance admission thresholds. Source faults stop LOS only.

## Conversion and ordering

Euler and Report are independent optional values in one observation. Validate
each independently. Copy doubles and nanoseconds without narrowing. Report
requires pointing, pointing rates and image rotation; reject any nonzero
validity flags or supplied optional status/platform values because masks are
unknown. Leave upstream optional defaults with validity zero: absence, **not**
false measurements. Finite values only; never clamp.

The LOS worker delivers Euler then Report for one observation, with no ordering
promise relative to image, Health or command threads. Detach either closes
admission or drains a callback already reserved under the registration mutex.
No unlimited queue; latest pending wins while a consumer is busy. No initial
placeholder or registration-time replay. Callback registration before enable
is supported; duplicate/empty/context rules reuse ChannelCore.

## Qualification plan

Add an SDK-free recording decorator backend, an independent exact-payload
oracle and barrier-based lifecycle/time tests using the real MEL channels.
Run focused LOS and public loader checks first, then the nine requested lanes,
installed consumers, isolation, integrity and publication guards. Production
Aravis/Pleora/GenTL mappings, navigation receipt, frame headers, buffers and
timestamp admission remain untouched. Record actual results separately from
physical sensor/frame alignment, UTC or complete MEL qualification.