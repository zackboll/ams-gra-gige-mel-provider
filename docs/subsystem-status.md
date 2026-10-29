# Bounded provider software subsystem status

This is a **partial, experimental native MEL profile** for Health
`SubsystemStatusResp`, implemented in the common provider runtime for every
backend. It describes one **software** subsystem whose single CSCI is this
provider software component. It is not a physical component inventory, camera
or SDK connection probe, hardware-health verdict, safety rating or full
Health/MEL conformance. No SDK/camera combination is qualified.

## Surface

Health advertises `ChannelMetadataCapabilityType::SubsystemStatusResp` in both
Control discovery and the attached channel's capabilities, alongside
`ChannelCommsTestRep`, `MFAStatus`, `MFAStatusDetailed` and `BITStatus`. C2 and
Image do not advertise it. Identities, factory signatures and backend interfaces
are unchanged. Health still reports zero image geometry and accepts no buffers.

The exact upstream overload
`registerMetadataCallback(std::function<void(Channel&, const SubsystemStatusResp* const)>)`
follows the existing Health registration contract: first valid succeeds, empty
returns BadPointer, duplicate fails, and any-provider-callback, detached or
closing state fails. Control/channel lifetime pins match BIT_Status. LFStatus,
DiscreteStatus, MFA_SecurityAuditRecord and NUC_TempData remain NotSupported
and unadvertised. Inherited common communications remain supported.

## Static inventory

| Field | Profile value | Meaning |
| --- | --- | --- |
| `subsystemId` | `1` | Scoped to this manager's provider software subsystem; not a globally allocated physical identifier |
| `criticality` | `0` | **Unassigned in this profile**; not a safety or mission-criticality rating |
| `subsystems` / `subsystemCount` | empty / `0` | No dependent subsystems are reported; count matches the vector |
| `csci` / `csciCount` | one entry / `1` | Count matches the vector |
| CSCI `csci` | `ams-gra-gige-mel-provider` | The provider software component |
| CSCI `criticality` | `0` | Unassigned, as above |
| CSCI `version` | `Version(0, 0, 1, 0)` | Provider software version 0.1.0 |

The pinned constructor order is
`Version(source, majorRevision, minorRevision, engineeringRevision)`. Source `0`
is unassigned in this profile. Project major/minor map to major/minor revision
and the project **patch** component maps to `engineeringRevision`. The values come
from the CMake project version through private compile definitions, the same
source as the existing MFA_Status About text `0.1.0` and the VersionInfo
description. They are distinct from the MEL API version (`VersionInfo` API 4 and
numeric library field 1, both unchanged) and from pinned dependency revisions.

## Current-state mapping

All fields are derived from the **same captured core state** used by the
existing MFA_Status/MFA_StatusDetailed reports in that publication.

| Captured state | CSCI mode | Failure (aggregate and CSCI) | connectionEstablished |
| --- | --- | --- | --- |
| Uninitialized (defensive; not normally publicly attachable) | `Unknown` | `NA` | `false` |
| Initialized, configuration known valid, no owned Image acquisition | `Idle` | `Available` | `true` |
| Initialized, configuration known valid, owned acquisition without observed terminal failure | `Imaging` | `Available` | `true` |
| Initialized, configuration invalid **or** currently owned acquisition failed | `Unknown` | `Major` | `true` |

The aggregate subsystem and its single CSCI always carry the same Failure value.
`Major` means this profile currently **cannot provide its image acquisition
function**. It is not a physical hardware diagnosis. `Critical` (annotated
upstream as "no heartbeat") is never used as a substitute for a transport or
acquisition error. `Available` keeps the existing Health limitation: it does not
guarantee that a later Image enable will succeed (buffers may be missing, held or
undersized, or the backend may fail).

`connectionEstablished` is true exactly when the initialized in-process provider
software association exists. It does **not** report camera reachability, an SDK
transport connection, heartbeat reception, packet delivery or successful future
acquisition. A camera/stream fault does not by itself destroy the association.

Historical state does not affect current readiness. Cumulative counters,
`last_error_history`, ordinary command rejection, transient receive timeouts,
pool pressure, contained metadata callback exceptions and a retained **failed
BIT completion** alone never produce `Major`. Explicit recovery (Image disable,
valid configuration readback) restores `Available` while that history remains.


## Counters

Both counters are manager-lifetime `uint32_t` values guarded by the runtime state
mutex. Each manager's counters are independent, and they survive Health
disable/detach/reattach and Control recreation while the runtime does. Both wrap
explicitly modulo 2^32 using unsigned arithmetic, with no signed overflow or
saturation. A compile-time assertion covers the wrap boundary. No public setter,
diagnostic ABI or production test hook exists.

### `BIT_report`: committed BIT-state revision

The upstream comment requires this counter to change when BIT_Status changes. It
starts at `0` for the empty canonical BIT state (no completion, no current faults).

It is committed by the existing `health_snapshot()` BIT preparation transaction:
the complete candidate BIT payload, completion and fault observations are formed
first, then committed together with the revision. One successful commit advances
the revision **exactly once** when either or both of these occur:

- A newly completed accepted BIT execution replaces the retained completion.
  This counts even if its Pass/Fail result, command ID or host timestamp repeats.
  An accepted execution whose test result is Fail is a successful execution.
- The observed current-fault state changes: first observation, observed
  retirement, or observed recurrence.

It does **not** change for an unchanged periodic sample, callback registration or
report delivery, a rejected command, a callback exception, historical
counter/diagnostic changes, or a configuration-generation change that leaves BIT
state unchanged. A preparation failure before commit preserves the previous
completion, fault observations and revision. Callback suppression or exception
after commit never rolls it back.

The revision tracks **committed observed state** and execution replacements. It
is not a lossless event log: fault transitions that occur entirely between two
observations coalesce under the existing observation model. Observations are C2
execution, C2 BIT_Status registration, and Health samples that need BIT state. A
background Health observation followed by a C2 execution can legitimately
produce two separate commits.

### `statusSeqNum`: prepared subsystem-report sequence

The stored value starts at `0`, so the first successfully prepared
`SubsystemStatusResp` carries `1`. Each later prepared subsystem snapshot
consumes the next value even when its contents are unchanged. Preparing other
report types (MFA_Status, Detailed, BIT_Status, C2 BIT reports) consumes none.
Re-enable or reattachment does not reset it. A prepared report may be suppressed
by admission/shutdown, or its callback may throw; the resulting **gaps are
valid**.

It is not a BIT revision, a state-transition counter, a physical heartbeat
monitor or a packet-loss measurement.


## Publication, coherence and callbacks

The existing Health worker publishes on its nominal, best-effort **100 ms**
interval and uses its prompt mechanism. Registration while disabled publishes
nothing. Enable prompts the first report, and late registration while enabled
prompts the existing worker, never inline dispatch. Subsystem-only registration
works because BIT state is prepared whenever either BIT_Status or
SubsystemStatusResp is registered. A Subsystem-only subscriber therefore still
observes fault changes and BIT_report revisions.

One captured core state serves the whole batch, in the order **MFA_Status,
MFA_StatusDetailed, BIT_Status, SubsystemStatusResp**. Each callback is admitted
independently, so disable/detach may suppress an unadmitted suffix. The revision
is captured with the snapshot and dispatched from it; dispatch never rereads the
latest runtime revision. An older batch held inside a consumer callback
therefore still delivers a BIT_Status and BIT_report that describe the same older
capture, even if C2 executes a newer BIT meanwhile. C2 does not wait for Health
callbacks, and no global callback order is imposed across C2 and Health. An older
Health batch may validly arrive after a newer C2 result.

No callback runs under operation or state locks. The snapshot path makes no
backend/device/stream calls and adds no worker. These existing protections apply
unchanged: acquisition-owner teardown ordering, weak worker ownership across
waits, self-join protection, external detach/disable drain, same- and
cross-provider lifecycle/configuration reentry rejection, callback-safe reads,
communications and explicit buffer release, and per-callback exception
containment (counted in `callback_failures`; Image is never faulted).

## Preparation failure

If canonical BIT preparation fails, the committed BIT state and revision are
preserved and both BIT_Status and SubsystemStatusResp are suppressed for that
sample. Status/Detailed publication continues, and `Health BIT publication
preparation failed: …` is recorded. One trigger is a new fault whose detection
time needs the checked host UTC clock while that clock is unavailable. No fresh
subsystem failure fields are ever paired with a stale revision, and a later valid
sample recovers. Tests **execute** this path through a test-executable clock
interposer.

A later failure while constructing the optional Subsystem payload (for example
an allocation failure) neither erases the already valid canonical BIT commit nor
suppresses the other prepared reports. It consumes no subsystem sequence and
records `Health subsystem publication preparation failed`. This branch is
**source-reviewed only**, because no production hook exists to inject it.

## Qualification limits

Only software tests apply. No hardware, SDK, transport performance, physical
health, component inventory, safety criticality or connection probing is
qualified. See the [validation record](subsystem-status-validation.md). The two
deferred PR #3 notes (fallback text for empty backend rejections; ConfigSet
UTF-8 BOM policy) remain separate follow-ups.

