# Bounded C2 ModeCmd: commanded Standby (GIGE-MEL-014 / GIGE-MEL-017)

This is a **partial implementation of a required MEL operation**, not complete
ModeCmd conformance or an operational task-scheduling contract. It uses the
unchanged IR MEL `8d9224519f12b44e0b28815755c56a32a28d24a0` and Common MEL
`f6908437d8fd2f7fb69896f9eb9cfd272d10c439` declarations. The response is
`RequestFor<irmel::MFA_Mode>`, not a new provider-specific response type.

## The one supported request

```cpp
ams::iface::irmel::ModeCmd command;
command.setCommandID(42); // every uint32_t value, including 0 and UINT32_MAX
command.setState(ams::iface::mel::MFA_State::Standby);
command.setMode(ams::iface::irmel::MFA_Mode::Unused);
// Leave ScanParam at its default/unset values.
auto future = c2->send(command);
```

Acceptance requires initialized Control, an attached/usable C2, known-valid
configuration and execution outside every provider
callback on the calling thread (including callbacks for another manager).
An enabled C2 is not required, consistent with the other synchronous commands.

### Idle: idempotent acknowledgement

When no Image acquisition is owned, the provider is already in observed Standby.
The preserved GIGE-MEL-014 acknowledgement:

- performs **no backend/device I/O**, connectivity probe or readback;
- does not call `ImageChannel::disable()`, create/stop a stream, or release buffers;
- does not modify configuration, configuration validity or generation;
- does not change Health state, clear diagnostics, or change BIT completion,
  faults or revision;
- does not schedule a task, start a worker, defer work, enqueue or use a timer.

### Active: actual logical acquisition transition

With a provider-owned Image acquisition, valid Standby stops frame admission,
requests stream cancellation, wakes and joins the receiving worker, stops the
backend stream and unpublishes acquisition ownership. It reuses the same private
`ChannelCore::stop_locked()` implementation as Image disable, with operations
already held; it never recursively calls public `ImageChannel::disable()`.
The future and Accepted status are formed **after shutdown completes**.

Image remains disabled, attached and reusable. Registered buffers, outstanding
consumer leases and their bytes survive. Consumers must explicitly release held
buffers before re-enabling; no command releases, unbinds or detaches them.
Shutdown waits for any already-entered Image callback to return. There are no
later deliveries from that acquisition after command completion. A subsequent
explicit enable may start a new acquisition.

### Faulted but still owned: explicit cleanup

A worker's terminal failure does not itself retire acquisition ownership.
Valid Standby now performs the same explicit cleanup as Image disable, including
when the worker has already exited. Removal of the current owned fault follows
existing shutdown and Health/BIT observation semantics. Stream-failure counters,
last-error history and retained BIT completions are not cleared. Current faults
retire only through the existing BIT recovery/observation policy.

Neither active nor idle acceptance changes camera configuration, validity or
generation. It creates no task, calibration, NUC, LOS, worker, queue or timer.
This commands **logical provider acquisition state**, not physical camera
standby, power management, reconnection, reachability or hardware health.

The exported factory remains synthetic-only. The same operation on an internally
injected Aravis provider acknowledges **cached provider readiness**, not camera
presence, physical health or future acquisition success. Production Aravis
timestamps remain `FrameTimestamp{}`; there is no physical-camera MEL profile.

## Validation and rejection

Admission precedes payload validation, following existing configuration/BIT
policy: provider-callback reentry returns `InvalidState` immediately, without
waiting for the operation lock or recursively dispatching CommandStatus. A
closing/destroyed Control or detached C2 returns `InvalidState` without command
dispatch; invalid payloads cannot override that precedence.

After admission:

| Condition | Future ErrorCode | Rejected CannotComply |
| --- | --- | --- |
| NotSet, MaxExclusive or out-of-range state; out-of-range mode; incompatible state/mode pair | InvalidParameters | InvalidInputParameter |
| Standby with any non-default ScanParam field (including nonfinite numbers) | InvalidParameters | InvalidInputParameter |
| Other defined upstream state/mode pair | Unsupported | CapabilityUnavailable |
| Valid Standby/Unused/default-scan but uninitialized or actual configuration not known valid | InvalidState | StateOrSettings |

The pinned modes `TaskSched`, `ScanVolumeSched`, and `ScanBarSched` are
operational-only; this profile treats Operate, OperateRxOnly and OperateTxOnly
as operational states requiring a non-Unused mode. All other defined states
require Unused. Operate/Unused and Standby/TaskSched are structurally invalid.
Unknown is a defined upstream value but unsupported as a requested state;
NotSet is an incomplete request. Defined but unsupported combinations include
Off, Not_Installed, Pre_initialization, Initialization, Maintenance, Calibration,
Initiated_BIT, Shutdown and Degraded with Unused, and all operational states
with each operational mode. Nothing is powered off, calibrated, scanned or
tracked. In particular, returning TaskSched would **not** implement scheduling.

### Exact ScanParam rule

For Standby, compare **every field** using the pinned getters against a
default-constructed upstream `ScanParam`/`ScanType`, including:

| Fields | Required default |
| --- | --- |
| `getIsDefinedWithRangeAndAltitude()` | false |
| `getCenter().az`, `.el` | finite zero |
| `getCentFrameRefEl()`, `getCentFrameRefAz()` | Inertial, Aircraft respectively |
| `getScanWidth()`, `getScanHeight()`, `getScanRate()` | finite zero |
| `getPreferredRevisitInterval()`, `getRequiredRevisitInterval()` | finite zero |
| `getScanId()` | 0 |
| ScanType `getContinuousScan()`, `getReturning()`, `getAgileScan()` | all 0 |
| `getMaxRangeOfInterest()`, `getMinRangeOfInterest()` | both 0 (documented unset) |
| `getElevationScanCenterAltitude()`, `getElevationScanCenterRange()` | both 0 (documented unset) |
| `getDegredationType()` (upstream spelling) | CAPACITY_DEGRADATION |

Numeric equality to finite zero rejects NaN and positive/negative infinity;
signed zero is equivalent to zero. No tolerance, normalization, inferred
geometry or ignored non-default field is used. ScanParam is meaningful upstream
for ScanVolumeSched, which remains unsupported here. No scan instructions are
executed and no vendor-specific geometry is defined.

## Future and CommandStatus correlation

Each admitted ModeCmd forms exactly one terminal outcome, independently of
whether a consumer has registered CommandStatus:

- Accepted: a ready future containing a non-null `MFA_Mode::Unused`; status
  `Accepted`, `CannotComply::NotSet`, empty rejection description.
- Rejected: a ready typed MEL error future (not an exception, null success,
  negative success payload or unfulfilled promise); status `Rejected`, the
  mapped reason above and a nonempty explanation.

`commandID` is copied exactly; it is not an execution key or deduplication ID.
Repeated IDs are separate idempotent acknowledgements. There is no Received
intermediate status, duplicate terminal completion, deferred queue or replay to
a callback registered later. Existing `ConfigurationOutcome`, `command_reason`,
`map_error` and `on_command_status` provide the mapping and dispatch. The future
is prepared before synchronous status dispatch and is ready when send returns.

As for existing C2 commands, external detach may close callback admission between
outcome preparation and dispatch, suppressing delivery; it cannot retract the
already formed future. Detach drains already admitted callbacks. Callback
exceptions are contained and counted by the existing policy; they cannot change
the command result or undo completed shutdown. Their diagnostic is new history, not a command
side effect or proof of a current camera fault.

## Observed state and concurrency

`Runtime::operations` serializes this transaction with configuration, Image
enable/disable, attach/detach and Control teardown. The existing `state_mutex`
protects the readiness/acquisition snapshot. There is no competing commanded
state variable. The same core facts continue to drive published Health
`MFA_Status`: initialized, valid and idle is Standby; an owned nonfaulted
acquisition is Operate; invalid configuration or a faulted owned acquisition is
Degraded. Health remains periodic best-effort sampling, not a command event log.

Even if the worker has already exited, faulted acquisition ownership remains
active until explicit Image disable or valid commanded Standby.
Shutdown/recovery never silently resets history. An acceptance linearizes after
shutdown (or at an idle snapshot); a later concurrent enable may legitimately change the
observed state before callback delivery. No acceptance claims idle state forever.

Control/C2 lifetime pins cover result formation and dispatch; an Image pin covers
the lock-held stop implementation. State/callback mutexes are never held while
joining the worker or stopping the stream. All consumer
callbacks run outside operation, state and callback mutexes. Provider-callback
reentry is an admission rejection, not a newly dispatched command, so it does
not cause recursive CommandStatus or wait for its own callback to finish.

## Permanent evidence and remaining work

The existing provider contract executable covers repeated/no-I/O
acknowledgement and generations/history, exact IDs, active/faulted shutdown,
invalid configuration and recovery, every defined state/mode pair, invalid
enums, every scan field and all seven floating fields with NaN and both
infinities, callback reentry/throw, closed channels and deterministic gated
lifecycle/configuration/teardown overlap. The additional watchdog-gated scenarios
observe cancellation and actual pthread join, status-after-worker-exit, repeated
idle commands, retained bytes/release/restart, simultaneous Standby and both
operation orderings. The same shared retained-buffer transition runs on synthetic
and the independent fixture; real Aravis Fake pixels use only the existing
test-only virtual clock. No CTest target or sanitizer suppression was added.

The public-header-only demonstration is shared by the native loader and the
installed consumer. Run the **same feature loader** against separately built,
unchanged starting-main and feature libraries:

```sh
# Use the same feature-built executable for both library paths.
/tmp/gm17/free-dev/loader_tests /tmp/gm17/baseline-build/libgige_mel_provider.so --expect-no-mode
/tmp/gm17/free-dev/loader_tests /tmp/gm17/free-dev/libgige_mel_provider.so --mode-only
```

The first verifies unchanged starting main `cb070729039677f29f65a564be0cc5fe1ad483df`
returns InvalidState/Rejected/StateOrSettings for **active** Standby, then explicitly
disables. The second verifies active Unused/Accepted and observed idle Health.
Both attach Image and C2, register a buffer, receive and retain a frame, check
correlation, preserve held bytes, explicitly release, re-enable for a second
capture, and disable/detach. The loader receives its Buffer through the existing
`dlsym` factory; the installed consumer uses the unchanged upstream factory.

Separately, the pinned metadata capability enum lacks `DiscreteStatus` and
`MFA_SecurityAuditRecord` entries. This task implements neither callback and does
not alter declarations or repurpose reserved values.

Full required ModeCmd work still needs a defined operational state machine and
real task-scheduling admission, execution, activity/status, cancellation,
capacity, timing and teardown contracts. Scanning, tracking, shutdown,
calibration, power management, navigation and physical-camera qualification
remain out of scope. See [the roadmap](mel-surface-roadmap.md) and
[validation record](validation.md).