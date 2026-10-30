# Bounded provider-software BIT profile

This is a **partial, experimental native MEL profile**, implemented in the common
provider runtime for all backends. It is not hardware BIT, full Health/MEL
conformance, or a physical connectivity certificate. No SDK/camera is qualified.

## Surface and catalogue

C2 implements `send(BIT_Command)`, `mel::BIT_Configuration` registration and
`mel::BIT_Status` registration, with correlated `CommandStatus`. C2 advertises
`BITConfiguration` and `BITStatus` in addition to existing communications and
CommandStatus capabilities. Health adds `BITStatus` to communications, MFAStatus
and MFAStatusDetailed. Image/C2/Health identities and factory signatures are
unchanged. Other unsupported overloads, including ModeCmd and navigation, retain
their existing behavior.

There is exactly one test, **Provider observed-state check**:

| Field | Exact profile value |
| --- | --- |
| Numeric `BIT_Command` ID | `1` |
| Profile / UCI descriptive label | `gige_mel.provider_state.v1` |
| Full 16-byte UCI UUID | `9c85c263-546e-4e11-9b71-d2405a138001` |
| UUID bytes in order | `9c 85 c2 63 54 6e 4e 11 9b 71 d2 40 5a 13 80 01` |
| Accepted interface | `BIT_ControlInterface::SubsystemBITCommand` |
| Physical component IDs | Empty |
| Expected duration | Unspecified: pinned `BIT_Type` default `nanoseconds{0}` |

Numeric ID 1 maps **explicitly** to this full UCI identity in catalogue and
completion records. Labels are not integer parsers. Identities are scoped to this
manager-local experimental profile, not a standardized cross-vendor mapping,
distributed identity allocation scheme, or physical inventory.

Exactly two logical item names appear in the catalogue and each completion:

1. `gige_mel.configuration_known_valid`: Pass only when initialized and current
   configuration is known valid by the existing apply/readback contract.
2. `gige_mel.owned_acquisition_without_terminal_failure`: Fail only when Image
   acquisition is logically owned and its captured terminal-failure flag is set.
   Idle and owned-without-observed-failure both Pass.

The overall result is Pass only if both pass. Failed items and overall failure
have useful nonempty reasons; passing items/results have empty failure reasons.
One coherent state capture reads acquisition failure **once**, independent of
historical counters, `last_error_history`, ordinary command rejection, pool
pressure, transient receive timeouts, and contained metadata callback exceptions.
An Image callback exception that marks acquisition failed does affect item 2.

Execution is synchronous, bounded to this fixed state/report size, adds no thread
and performs no enumerate/open/configuration/apply/create/start/receive or other
backend/device/stream I/O. It works idle, running and logically owned after
terminal failure. It changes no camera configuration/generation, acquisition
lifecycle, registered buffer or retained image byte. A Pass does **not** establish
physical connectivity or guarantee a later enable: missing/held/undersized buffers
or a future backend failure may still prevent acquisition.

## Operation selection and outcomes

The first nonempty operation wins: **initiate, then cancel, then clearFaultCode**.
Only the selected operation is validated. Lower-priority fields are ignored,
including malformed/unknown contents. There is no fallback after selected-operation
rejection. A selected initiate/cancel must have exactly one ID; length is checked
before ID lookup, so even repeated `1` entries are invalid parameters.

| Selected operation / outcome | Ready future | C2 terminal status / CannotComply |
| --- | --- | --- |
| Initiate `{1}`, report formed (even test Fail) | `Return::Success` | Accepted / NotSet, empty rejection description |
| All empty, or selected initiate/cancel length not 1 | InvalidParameters, diagnostic | Rejected / InvalidInputParameter |
| Unknown singleton initiate/cancel, including 0 | InvalidId, diagnostic | Rejected / UnknownID |
| Cancel `{1}` | InvalidState, `no active test` diagnostic | Rejected / StateOrSettings |
| Selected clearFaultCode (any nonempty vector) | Unsupported, diagnostic | Rejected / CapabilityUnavailable |
| Internal report/timestamp preparation failure | InsufficientLocalResources, diagnostic | Rejected / InsufficientResources |
| Any-provider callback reentry, detached C2, closing Control | InvalidState, diagnostic | Pre-admission rejection: no recursive CommandStatus or BIT_Status |

A valid command executing an unhealthy test is still **accepted and successful**;
test outcome is separate from command execution. There is no queued/running BIT,
asynchronous scheduling, simulated progress, cancellation success, or Interrupted
record. `activeBITs` is always empty. C2 enable/disable neither creates nor cancels
BIT work; attached C2 accepts requests even when never enabled. clearFaultCode
never clears faults, counters, diagnostics or results; real recovery governs
current software predicates.

## Latest completion versus current faults

Runtime retains at most one immutable `CompletedBIT`: the latest successfully
executed instance. Before the first execution the list is empty; a successful
execution replaces rather than appends. Rejected requests and internal preparation
failures preserve it. C2/Health detach/reattach and Control recreation within the
same manager do not erase it. A new manager starts empty.

`BIT_Status` has no command execution ID. `CommandStatus.commandID` correlates
acceptance only. This profile promises no persistent per-command history,
duplicate-ID suppression, or extra execution correlation absent from pinned types.

Exactly two current software fault categories are possible:

| Fault code and UCI descriptive label | Complete category UUID | Predicate |
| --- | --- | --- |
| `gige_mel.configuration_invalid` | `9c85c263-546e-4e11-9b71-d2405a138011` | Initialized and configuration_valid false |
| `gige_mel.acquisition_failed` | `9c85c263-546e-4e11-9b71-d2405a138012` | Initialized and acquisition owned and captured terminal failure true |

UUID bytes are the test's first 15 bytes followed by `11` or `12` respectively.
Included faults have `FaultState::Set`, `FaultSeverity::Warning` (consistent with
Degraded), and useful observed-software-condition descriptions. `faultData`,
component IDs and ambiguity groups are empty: no hardware diagnosis is invented.
Defensive uninitialized state stays Unknown with no fabricated configuration
fault. Faults derive from fresh state, **not** latest completion, cumulative
counters or last error. Valid configuration recovery removes the configuration
fault; explicit Image cleanup removes the owned-acquisition fault.

A bounded two-category observation record, updated under the state mutex,
preserves first-observation detection time while a predicate remains observed set.
An observed clear resets that category; a later observed recurrence gets a newly
acquired detection time. IDs identify categories, not incident records. Sampling
can coalesce transitions between observations; there is no complete event history.
A failed last completion intentionally remains after current faults disappear,
until another accepted test replaces it.

## Linux time convention and failure handling

Completion and detection times are **Unix-epoch nanoseconds from the host system
UTC wall-clock estimate**, acquired using `clock_gettime(CLOCK_REALTIME)`. Completion
means item evaluation finished. Detection means the first observation by this
reporting facility in an uninterrupted *observed* set sequence, not reconstructed
transport-event time. This introduces no PTP, synchronization, traceability or
accuracy guarantee, and changes no image timestamp contract (synthetic images
still use their non-UTC monotonic clock).

Negative/out-of-range values, zero (pinned unspecified representation), invalid
nanosecond fields and clock syscall failure are errors, never replaced by zero or
monotonic time. Conversion checks bounds before multiplication/addition. Report
preparation commits completion/observation state only after the full payload is
formed. A failure rejects initiation with a diagnostic and preserves completion;
initial C2 status registration fails rather than publishing invented data. If
Health BIT preparation fails, existing MFA_Status and MFA_StatusDetailed remain
eligible, BIT is suppressed and the preparation diagnostic is recorded. UTC need
not increase across host clock adjustments. A continuing fault retains its exact
recorded detection time even if the host clock moves.

## Publication, concurrency and lifetime

C2 catalogue and status registration: first valid succeeds; empty callback returns
BadPointer; duplicate, any-provider-callback context, detached or closing state
fails. Successful registration **can synchronously invoke** its initial callback
on the registering thread, outside provider locks. Initial status has truthful
current faults and an empty completion list if no test has executed. There is no
periodic C2 publisher.

An accepted initiation captures its own immutable result/status payload in the
transaction and prepares a ready future before consumer entry. After releasing
operation/state locks, it delivers C2 **BIT_Status then CommandStatus**. Admitted
ordinary rejection delivers only its correlated rejection CommandStatus. No later
read of the latest completion substitutes a concurrent command's result.

Transaction serialization is not total callback ordering across concurrent C2
callers. Detach may suppress callbacks not yet admitted; it drains those already
admitted. The future remains authoritative. Strong Control/channel pins protect
all synchronous registration and command dispatch, including final external owner
release inside callbacks. Consumer strong-capture cycles remain the consumer's
responsibility; keep the DSO loaded through all objects/control blocks/workers.

Health uses its existing worker and nominal **100 ms**, best-effort sampling,
without the operation mutex or device I/O. BIT-only and late registration while
enabled use its prompt mechanism. No Health callback runs inline from C2 or while
Health is disabled. Registered report order is **MFA_Status, MFA_StatusDetailed,
BIT_Status** from one core capture and consistently captured retained completion.
Each callback has independent admission; shutdown may suppress a suffix. The
twelve detailed keys and state mapping are unchanged. Slow callbacks delay later
samples; no catch-up/history queue is added. The [subsystem profile](subsystem-status.md)
appends SubsystemStatusResp to that batch and commits a BIT-state revision with
each semantic BIT-state change; BIT identities, items and categories are unchanged.

No callback runs under provider locks. Exceptions are independently contained and
counted/reported, preserve future and stored completion, and allow later eligible
callbacks. Metadata exceptions never create an Image failure. Same- and
cross-manager mutation attempts reject before operation locking and without
recursive status delivery. Callback-safe reads, communications, keep-alive and
explicit buffer release remain usable. Health ownership remains weak across waits
with scoped strong preparation/dispatch pins; no new cycle/self-join path is added.
The R1 acquisition-unpublication-before-owner-release ordering is retained.

Hardware/firmware, physical connectivity, packets, memory tests, calibration,
physical components and asynchronous BIT are future separately qualified work,
not aliases for this software check. The two deferred PR #3 notes remain separate.
See [validation](software-bit-validation.md) for actual executed evidence.