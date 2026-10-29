# Partial provider Health and diagnostic snapshots

This is a **partial Health implementation** for the experimental Mono8
image-stream profile, not full Health or MEL conformance. It reports observed
software-provider facts, not physical-health certification. No hardware/SDK
combination, BIT, calibration, security audit, navigation, temperature, transport
packet statistics, UTC clock, or hardware inventory is qualified or fabricated.

## Public MEL surface

After successful Control initialization, discovery contains Image, C2, and
`ChannelType::HealthAndStatus`. Existing Image/C2 UUIDs remain unchanged. Health
uses manager-local UUID bytes all zero except final byte 3 and label
`<instance>/health`. Use its advertised identity and matching type; one Health
attachment per manager is allowed. No Image listener or buffers are required.
Reattachment after detach creates fresh callback-registration state.

The exact Health metadata capability set is `ChannelCommsTestRep`, `MFAStatus`,
and `MFAStatusDetailed`, independent of callback registration. All image geometry
and storage fields (width, height, pitch, image/buffer size, bit depth, bands)
are zero. No reserved capability enum is repurposed.

Implemented Health-specific registrations are `MFA_Status` and
`MFA_StatusDetailed`. The first nonempty callback succeeds; empty returns
BadPointer on a usable channel; duplicate returns Fail. Registration on a
detached channel or from **any provider callback on the thread**, including
another manager's callback, fails. There is no replacement or unregister API.
The other six registrations return NotSupported: LFStatus, BIT_Status,
SubsystemStatusResp, DiscreteStatus, MFA_SecurityAuditRecord, and NUC_TempData.

Inherited keep-alive and correlated communications remain available even while
Health reporting is disabled. Health accepts no image buffers: both register
and unregister return NotSupported on an attached usable channel, without
retaining or modifying the supplied buffer. Callback-context and detached calls
retain their established Fail rejection precedence.

## Current state, not diagnostic-history inference

The settled-state mapping is evaluated from one captured core snapshot:

| Observed condition | MFA_State |
| --- | --- |
| Configuration is not known valid | Degraded |
| Valid configuration, owned Image acquisition with observed terminal failure | Degraded |
| Valid configuration, owned Image acquisition without observed terminal failure | Operate |
| Initialized, valid configuration, no owned Image acquisition | Standby |
| Defensive uninitialized case (not normally publicly attachable) | Unknown |

All reports use `StateTransitionStatus::NotTransitioning`. State descriptions
explain these observations; the human-readable mode describes the experimental
Mono8 image-stream profile and explicitly disclaims TaskSched, scanning, BIT,
calibration and ModeCmd. `MFA_Status` has no timestamp, command ID or typed mode
field. About identifies `ams-gra-gige-mel-provider (software provider)` version
0.1.0. Physical serial, hardware version and bootloader version are empty. The
logical device selector is **not** a physical serial. The component list is
empty because this profile does not report components, not because hardware
is asserted to have none.

**Standby** means idle with known valid configuration and able to accept an
explicit Image enable attempt. It does not prove a disconnected camera has
reconnected. **Operate** means logically owned acquisition without an observed
terminal fault; it does not promise a frame in each interval. **Degraded** means
configuration invalidity or a current owned acquisition fault, not a general
hardware-health verdict.

Terminal receive errors (including unexpected stopped), clock-contract failures,
and Image listener exceptions mark the owned acquisition failed. Listener
exceptions increment callback_failures, not stream_failures. Explicit Image
disable removes that acquisition and its current failure; repeated enable while
faulted remains rejected. Invalid configuration remains invalid until actual
valid readback establishes readiness. Valid readback after partial apply failure
is represented accurately even though that command failed.

During Image cleanup, the provider retains a strong acquisition owner through
worker shutdown and stream stop, clears `image_active` and the weak acquisition
publication together under the state mutex, then releases its stream owner
outside that mutex. A fresh Health snapshot after this unpublication reports
idle/Standby (when configuration is valid), even if backend stream destruction
is still finishing. This avoids a false running/Operate interval and prevents
ordinary external cleanup from transferring final stream destruction to Health
sampling under the state mutex. Enable's exception cleanup unpublishes both
fields while its local acquisition owner still exists. Neither Health nor C2
disable clears Image state because cleanup retains the owned-stream guard.

Neither nonzero historical counters nor last_error imply Degraded. Ordinary
command rejection, receive polling timeout, no-buffer drops, nonterminal rejected
frames and contained Health callback exceptions do not independently degrade
current acquisition. Counters and last_error survive Health disable/detach and
successful recovery. A failed create_stream/start attempt that retains no
acquisition remains **idle**, with the failure in diagnostic history. This is an
observation limit, not a reconnect probe or new fault-latching policy.

## Detailed-status schema 1

Every report contains exactly these 12 unique provider-defined names, in this
order. Strings contain no invented units or timestamps. Integer values are
locale-independent unsigned decimal, without grouping or a sign.

| Name | Value / meaning |
| --- | --- |
| `gige_mel.health_schema` | Literal `1` |
| `gige_mel.backend_id` | Selected backend identifier, copied before publication starts |
| `gige_mel.device_id` | Configured logical device selector |
| `gige_mel.configuration_valid` | `true` or `false`: actual configuration is known valid |
| `gige_mel.configuration_generation` | Current provider configuration generation; advances after attempted apply/readback, not schema rejection |
| `gige_mel.acquisition_state` | `idle`, `running`, or `faulted`: acquisition ownership and observed failure, independent of configuration validity |
| `gige_mel.delivered_frames` | Cumulative frames admitted to Image listener delivery (including a listener that subsequently throws) |
| `gige_mel.dropped_no_buffer` | Cumulative frames dropped because no registered consumer slot is free |
| `gige_mel.rejected_frames` | Cumulative frames rejected by layout/completion/generation/time validation |
| `gige_mel.callback_failures` | Cumulative contained consumer exceptions across Image and metadata callbacks, including Health |
| `gige_mel.stream_failures` | Cumulative Image worker terminal receive, terminal clock and worker exception failures; not Image listener exceptions or create/start failures before ownership |
| `gige_mel.last_error_history` | Existing latest historical diagnostic text, possibly empty or retained after success |

Core configuration/ownership facts and diagnostic text are captured under the
runtime state mutex. The current acquisition's failure flag is sampled atomically
through a state-mutex-protected weak acquisition link. Counters are independent
atomic samples during capture, **not an instantaneous transaction across every
counter**. No global per-frame Health lock is added. Both messages in one
publication derive from this same capture, even if provider state changes between
callbacks. A callback exception affects later snapshots, not the already-captured
Detailed report in that publication.

## Publication, threading and shutdown

A new Health attachment is disabled. Registering while disabled publishes
nothing. The first successful enable creates one publisher thread and prompts
an initial sample; repeated enable is idempotent. Successful late registration
while enabled wakes it for a prompt current sample, never synchronously on the
registration thread. Re-enable preserves registrations and starts a fresh sample.

The nominal interval is **100 milliseconds**, using interruptible waiting after
dispatch. This is best effort, not real time. There is no history queue, catch-up
burst or exactly-once transition guarantee. Slow callbacks delay later reports;
intermediate states may be coalesced. Callbacks must return in bounded time.
Normal order is MFA_Status then MFA_StatusDetailed on the same publisher thread.
Each is admitted independently; concurrent disable/detach can suppress a callback
not yet admitted, so shutdown does not guarantee complete two-message batches.
Inherited communications callbacks remain synchronous on their submitting
threads and can run concurrently with the publisher.

Disable closes periodic admission, interrupts waiting and waits for the worker's
callbacks to finish. It does not stop Image, change configuration, release
buffers, or disable common communications. Detach additionally closes all
metadata admission and drains admitted callbacks, including communications.
Control destruction uses this same internal stop path for both workers.

No operation, runtime-state or callback mutex is held while entering consumer
code. The Health worker never takes Runtime::operations and performs **no
backend/device/stream I/O**, including configuration query/apply or stream
creation/reception. Lifecycle/configuration mutations from any provider callback
are rejected; ConfigSet/CameraCommand return InvalidState without recursive C2
status or backend application. Callback-safe reads, explicit Image buffer
release, keep-alive and communications retain their existing contracts.

Each Health consumer exception is independently contained, increments
callback_failures and records history. It does not fault Image or stop the
publisher; the other report can still run if admitted. There is no recursive
publication from an exception handler.

The worker uses independent state and weak owner links. Control and channel are
strongly pinned only during preparation/dispatch and released **before waiting**.
Dropping their last external references inside a callback is safe through its
completion; self-thread teardown detaches the thread handle and independent
worker state survives until exit. Prefer explicit orderly shutdown outside
callbacks. Avoid consumer-created strong capture cycles. The DSO must remain
loaded until every provider object/shared-pointer control block and active
callback/worker has finished; this feature does not change the DSO contract.

## Public client demonstration and negative control

The native loader and separate installed consumer use only published MEL headers
to discover/attach Health, register both callbacks, enable, wait for Standby and
schema-1 idle diagnostics, disable and detach. Existing image capture and
ConfigSet/correlated CommandStatus demonstrations remain in place.

The loader also accepts `--health-only` and `--expect-no-health`. The former must
fail against starting main with `missing required HealthAndStatus discovery
capability`; the latter proves baseline absence and rejects attachment using
the feature's UUID. Run the same feature-built loader against an unchanged
baseline library in a disposable worktree; do not patch baseline behavior.

## Deferred PR #3 review notes

These remain separate follow-ups, not prerequisites or incidental corrections:

- Fallback text for empty backend command-rejection diagnostics.
- Explicit leading UTF-8 BOM policy for ConfigSet.