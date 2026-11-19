# Source-backed virtual stare tasking (GIGE-MEL-045)

This deliberately limited profile is **not complete TaskSched or AMS GRA MEL
conformance**. Default synthetic, Aravis, Pleora and GenTL remain unsupported.
Only the explicitly composed SDK-free recording executor supplies this profile.
No production adapter or camera profile is changed.

## Discovery and operation

Optional MEL/SDK-independent `ITaskExecutionSource` declares immutable virtual
stare support, clock/origin, coordinates, duration limits, capacity, cancellation,
atomic replacement, identity, generation and provenance. Incomplete declarations
fail closed. Control advertises Scheduling only for the qualified profile, UUID
suffix **5** (Image 1, C2 2, Health 3, Stack 4 unchanged). Common Channel lifecycle,
communications and callback infrastructure applies.

`Operate/TaskSched` opens and observes an owned idle execution session before
acceptance; repeated entry preserves it. The exact result is `RequestFor<MFA_Mode>`
(future of ErrorOr of shared_ptr). Correlated terminal C2 CommandStatus is
reserved before releasing admission and delivered outside locks. No source means
Unsupported/Rejected/CapabilityUnavailable. Invalid state/mode combinations keep
their InvalidParameters behavior. ScanVolumeSched, ScanBarSched, OperateRxOnly
and OperateTxOnly remain unsupported. Scan fields must be unset.

## Exact conventions

| Input | Supported convention |
| --- | --- |
| Family | Virtual IRSTStareTask only |
| TaskInfo systemTime | Exactly 0 ns: immediate at source virtual now; never UTC/host time or future scheduling |
| Clock | Session-relative virtual; origin zero on new session, explicitly advanced by recording executor |
| Center | Exactly azimuth/elevation 0 radians, both references Aircraft; virtual platform-origin token, not measured aperture pointing |
| Duration | Positive nanoseconds within source limits; fixture 1 ns–10 s; provider rejects declarations beyond 60 s |
| Action | Immediate only; Normal/Next require queues and are Unsupported; embedded Abort is Unsupported, not a stare |
| Priority / precedence | Exactly 0; no priority scheduling |
| IDs | All uint32 values including 0 and UINT32_MAX; command/request IDs independently retained |
| Duplicates | InvalidId for previously accepted request ID in same session, including terminal tasks; command IDs may repeat |
| Session | At most source-declared admissions, bounded to 256; fixture 16. Exhaustion rejects without mutation; new session resets history |

No transforms, navigation-derived positions, images or physical gimbal commands.
Nonzero/NaN/infinite coordinates fail.

## Capacity and outcomes

TaskScheduleDepth zero means **one active task, no queue**, not unsupported
scheduling. Immediate admission atomically cancels old active work and owns the
new request. Failed validation, duplicate IDs, exhaustion or source admission
leaves the old task unchanged and emits no ack. A source lacking safe atomic
replacement is not advertised.

Source phases distinguish idle, admitted, executing, completed, cancelled and
failed. The fixture owns virtual time, advances admitted → executing explicitly,
and completes only after the requested duration in that clock.
`ExecuteTaskAck(commandID, reqId)` follows admission, **not completion**.
No asynchronous task metadata worker or queue is created.

The pinned Task enum has no stare entry or documented alias. IRSTPosition,
IRSTSTT, Idle and reserved values are not defensible substitutes.
TaskExecutingRep stays NotSupported/unadvertised. Source observations are exposed
at the source seam, not fabricated as MEL completion reports: an explicit public
outcome-reporting gap. SchedCreatedRep, TaskEvents, scan performance, tracking,
track lists and unrelated metadata remain unsupported/unadvertised.

## Cancellation, shutdown and lifetime

Pinned AbortTask describes current-task abort but has **no public getTaskInfo()
or request ID getter**. No hidden fields are examined. Only current cancellation
is supported, never targeted cancellation. Success establishes cancellation,
not merely its request. Idle/terminal abort fails. Cancellation failure retains
ownership for retry, Standby or mandatory teardown retirement.

Standby observes work, cancels admitted/executing work and retires the session
only on success. Observation/cancellation failure rejects Standby and retains
ownership. Terminal work retires without pretending it was newly cancelled.
Scheduling disable/detach and Control destruction force bounded retirement even
after source failure; the fixture records failed, not successful cancellation,
if cancellation fails during forced retirement.

Calls receive 100 ms steady-clock deadlines; retirement/destruction must finish
within 100 ms. These are cooperative source qualification requirements, **not**
forced interruption of arbitrary C++ code. No detached timeout worker exists.
Consumer callbacks may block detach until return, as on existing channels;
source shutdown and callback drain are separate contracts.

Task sessions and Image/Stack acquisition are mutually exclusive in either
order. Reconfiguration requires session retirement. Retained buffer bytes and
explicit release remain unchanged. Cancellation does not clear diagnostics,
BIT completions or revisions. Health Operate means an owned virtual admission
session (including between tasks), never physical operation. Existing acquisition
detail/counters remain image/stack-only; subsystem CSCI uses Operational.

Global callback-context rejection also covers other managers. Acks reserve
delivery under operation admission, run outside locks, contain exceptions and
drain on detach even before callback entry. Commands pin Control/channel through
delivery. No callback follows completed detach. Source results settle futures.
Tests use gates/barriers and explicit state rather than sleeps for ordering.

See [design](tasks/GIGE-MEL-045-design.md) and
[qualification](tasks/GIGE-MEL-045-validation.md).