# Bounded C2 ModeCmd acknowledgement (GIGE-MEL-014)

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
configuration, no owned Image acquisition, and execution outside every provider
callback on the calling thread (including callbacks for another manager).
An enabled C2 is not required, consistent with the other synchronous commands.

This is an **idempotent Standby acknowledgement**: the provider is already in
the observed Standby state. It does not command or perform a transition. It:

- performs **no backend/device I/O**, connectivity probe or readback;
- does not call `ImageChannel::disable()`, create/stop a stream, or release buffers;
- does not modify configuration, configuration validity or generation;
- does not change Health state, clear diagnostics, or change BIT completion,
  faults or revision;
- does not schedule a task, start a worker, defer work, enqueue or use a timer.

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
| Valid Standby/Unused/default-scan but uninitialized, invalid configuration, or owned Image acquisition | InvalidState | StateOrSettings |

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
the command result or stop Image. Their diagnostic is new history, not a command
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
active until explicit Image disable. Standby is rejected in that interval.
Disable/recovery never silently resets history. An acceptance linearizes at one
coherent idle snapshot; a later concurrent enable may legitimately change the
observed state before callback delivery. No acceptance claims idle state forever.

Control/channel lifetime pins cover result formation and dispatch. All consumer
callbacks run outside operation, state and callback mutexes. Provider-callback
reentry is an admission rejection, not a newly dispatched command, so it does
not cause recursive CommandStatus or wait for its own callback to finish.

## Permanent evidence and remaining work

The existing provider contract executable covers M1–M15: repeated/no-I/O
acknowledgement and generations/history, exact IDs, active/faulted ownership,
invalid configuration and recovery, every defined state/mode pair, invalid
enums, every scan field and all seven floating fields with NaN and both
infinities, callback reentry/throw, closed channels and deterministic gated
lifecycle/configuration/teardown overlap. No CTest target was added.

The public-header-only demonstration is shared by the native loader and the
installed consumer. Run the **same feature loader** against separately built,
unchanged starting-main and feature libraries:

```sh
# Use absolute paths to the executable and libraries in your build directories.
loader_tests /path/to/unchanged-main/libgige_mel_provider.so --expect-no-mode
loader_tests /path/to/feature/libgige_mel_provider.so --mode-only
```

The first verifies starting main's Unsupported Standby stub; the second verifies
Unused/Accepted, Operate/TaskSched typed Unsupported/Rejected, and observed idle
Health with zero delivered frames. Neither focused case attaches Image.

Full required ModeCmd work still needs a defined operational state machine and
real task-scheduling admission, execution, activity/status, cancellation,
capacity, timing and teardown contracts. Scanning, tracking, shutdown,
calibration, power management, navigation and physical-camera qualification
remain out of scope. See [the roadmap](mel-surface-roadmap.md) and
[validation record](validation.md).