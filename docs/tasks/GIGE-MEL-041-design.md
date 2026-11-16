# GIGE-MEL-041 — required Health metadata: contract analysis and classification

**Disposition:** `BLOCKED_BY_UPSTREAM_CAPABILITY_ENUM` for both
`DiscreteStatus` and `MFA_SecurityAuditRecord`. `MFA_SecurityAuditRecord` is
**also** blocked by a missing provider security event source.

Neither surface is implemented. Both registrations keep returning
`Return::NotSupported` and neither is advertised. This task adds executable
classification tests, a public-header consumer check and a prepared upstream
issue. It does **not** complete `HealthStatusChannel` and does not make the
provider MEL-conformant.

## Scope and pinned sources

The authoritative contract is the unmodified pinned closure in
[`third_party/manifest.json`](../../third_party/manifest.json): IR MEL
`8d9224519f12b44e0b28815755c56a32a28d24a0` and Common MEL
`f6908437d8fd2f7fb69896f9eb9cfd272d10c439`. Reviewed:

* `irmel/library/health-status/HealthStatusChannel.h`;
* `irmel/library/irmel-types/Channel.h`, `ChannelCapability.h` and
  `ChannelMetadataCapabilityType.h`;
* `mel/library/DiscreteStatus.h`, `NameValuePair.h` and `UCI_ID.h`;
* `mel/library/securityauditrecord/*`: the record, the base type, the six event
  types and `SecurityArtifact`.

Provider sources reviewed: the Health worker, BIT/Subsystem preparation, callback
admission/drain, detach and lifetime rules in `src/provider.cpp`, and the
[Health](../health-status.md), [software BIT](../software-bit.md) and
[subsystem](../subsystem-status.md) contracts.

## 1. Exact upstream declarations

```cpp
/// @Required ... must be provided by the implementer for all IR MEL implementations
virtual Return registerMetadataCallback(
    std::function<void(Channel& channel, const ams::iface::mel::DiscreteStatus* const)> callback) = 0;
/// @Required ... must be provided by the implementer for all IR MEL implementation
virtual Return registerMetadataCallback(
    std::function<void(Channel& channel, const ams::iface::mel::MFA_SecurityAuditRecord* const)> callback) = 0;
```

Both declarations document the same results:

* `NotSupported` if not implemented;
* `Success` if implemented and registration succeeds;
* `Fail` if a callback is already registered.

**`DiscreteStatus`** (`@Required`, "subsystem unique data that cannot be
reported with other messages") has exactly one field,
`std::vector<NameValuePair> status`. Each entry is a pair of free-form strings.
The structure has no timestamp, sequence, unit, schema or validity field.

**`MFA_SecurityAuditRecord`** (`@Required`, aligned with the OMS/UCI
SecurityAuditRecord) has these fields:

* `securityEventID` and `subsystemID` (`UCI_ID`);
* `eventTimestamp` (nanoseconds, clock domain unspecified);
* `securityArtifacts` (component/associated `UCI_ID` pairs);
* `eventType`, a variant of `monostate`, Authentication, Integrity,
  FileManagement, KeyManagement, System or Sanitization;
* `outcome` (`NotSet`, `Failure`, `Success`);
* `severity` (`NotSet`, `Critical`, `Error`, `Informational`, `Warning`).

The class comment says MFAs report only event types "relevant to their type and
design".

The new tests compile both exact overloads against the pinned abstract class,
using member-function-pointer selection on `ir::HealthStatusChannel`. They are
not recreated from prose.

## 2. Contract contradiction

The pinned `ChannelMetadataCapabilityType` (`std::uint32_t`) has named values
0–27 and `Reserved3`, `Reserved5`, `Reserved9`, `Reserved10` (28–31). It notes
that "each enum value should correspond to a registerMetadataCallback function".
`ChannelCapability` then says that values are "bounded by the available values
defined in the ChannelMetadataCapabilityType enumeration", and that a
registration without a corresponding entry is "expected to return
Return::NotSupported".

A scan of every pinned `registerMetadataCallback` payload against the
enumerators finds five payloads with **no** enumerator:

| Payload | Channel | Annotation |
| --- | --- | --- |
| `DiscreteStatus` | Health | **@Required** |
| `MFA_SecurityAuditRecord` | Health | **@Required** |
| `NUC_TempData` | Health and Image | Optional |
| `CalibrationConfiguration` | C2 | Optional |
| `CalibrationStatus` | C2 | Optional |

For the two required payloads, three statements cannot all hold at once:

1. `HealthStatusChannel` requires every implementation to provide them.
2. `ChannelCapability` expects an unadvertised registration to return
   `NotSupported`.
3. No corresponding entry exists, and the reserved values have no assigned
   meaning.

The consequences:

* **Returning `Success`** would contradict the provider's own capability report.
* **Advertising a reserved value** would violate the no-repurposing rule. It
  would also be indistinguishable from a future upstream assignment.

**No conformant implementation of either surface exists under the pinned
revision.** The truthful, self-consistent behavior is the current one: implement
the pure virtual, return `NotSupported`, advertise nothing, and record the
conformance gap.

Alternatives considered and not chosen:

* **Return `Success` without advertising.** Rejected by owner decision on this
  task. It contradicts the capability rule, and capability-gated consumers could
  never discover it.
* **Internal publisher behind a `NotSupported` registration.** Rejected. It is
  unreachable through the public API, untestable as MEL behavior, and dead code.
* **Use a `Reserved*` value.** Prohibited.

`NotSupported` takes precedence over lifecycle state. Unlike supported
registrations, which `Fail` when detached, closing or in a callback, an
unimplemented surface returns `NotSupported` for empty, repeated,
callback-context, detached and post-Control-destruction calls. That is
unchanged pre-existing behavior; the new tests pin it.

## 3. DiscreteStatus semantics analysis

`DiscreteStatus` can *represent* provider facts, because free-form string pairs
fit anything. The block is therefore the advertisement contract, not the payload.
A truthful mapping was still analysed so that it is ready if upstream adds an
enumerator.

The provider already owns and establishes these facts:

* configuration validity and generation;
* acquisition ownership and observed failure;
* BIT revision and current fault categories;
* cumulative counters;
* backend and device identifiers.

Each is already reported by `MFA_StatusDetailed` schema 1, `BIT_Status` or
`SubsystemStatusResp`. Upstream restricts `DiscreteStatus` to data "that cannot be
reported with other messages". Copying these facts would duplicate them under a
second, unversioned vocabulary, and would add a fifth report that must stay
consistent with the other four.

The provider has **no genuinely discrete signal** of its own. There are no
physical discrete inputs or outputs, camera GPIO or line states, interlocks,
hardware alarms or sensor temperatures, and the backend interface
(`include/gige_mel/backend.hpp`) exposes none. Inventing such entries is
prohibited.

**Candidate profile, held for upstream resolution and not implemented:**

* *Contents:*
  * a `gige_mel.discrete_schema` literal;
  * two provider software booleans already defined as BIT items,
    `gige_mel.configuration_known_valid` and
    `gige_mel.owned_acquisition_without_terminal_failure`.
* *Source:* derived from the same captured Health snapshot.
* *Delivery:* appended after `SubsystemStatusResp` in the existing batch, under
  the unchanged registration, admission, drain and lifetime contract.
* *Excluded:* any physical or SDK-derived entry.

Whether even this adds value beyond `MFA_StatusDetailed` is a profile question to
settle together with the enumerator.

## 4. Security audit record event-source investigation

| Schema category | Candidate provider event | Finding |
| --- | --- | --- |
| Authentication | None. There is no credential, signature, certificate or authenticated session in the provider, either backend or the profile loader | No source |
| Integrity | ConfigSet JSON schema validation; profile-module descriptor/build-identity checks (`src/selection/profile_loader.cpp`) | These are schema and ABI compatibility checks. There is no hash, signature or measured-boot evidence, so they are not integrity verification |
| File management | ConfigSet / CameraCommand apply | Volatile in-process camera configuration. There is no file, persistent update or authorization model. Recording these would make ordinary commands security events |
| Key management | None | There are no keys or certificates |
| Sanitization | `EraseCommand` | `Unsupported` in this provider. Nothing is erased |
| System (startup, shutdown, reset, fault) | `Control::init`, Control destruction, observed acquisition fault | Ordinary lifecycle and diagnostics, already reported by `MFA_Status`/`BIT_Status`. They are driven by an unauthenticated in-process consumer, involve no security-policy decision, and have no trusted event-ID allocation or declared timestamp domain. Relabelling them would be fabrication |

The following are ordinary command or diagnostic outcomes, not
security-policy enforcement points:

* command, schema and callback-context rejections;
* stream faults;
* contained consumer exceptions.

**Decision.** The provider has no source-backed security event. The callback
stays `NotSupported`, and no event queue, retention or overflow policy is
implemented. Two things are missing:

1. **A provider or deployment source.** This needs a real security-relevant
   mechanism, such as:
   * authenticated configuration or signature-verified module loading;
   * an implemented zeroization or erase;
   * a defined security policy for lifecycle events.

   It also needs a security event-ID allocation scheme and a declared
   `eventTimestamp` clock domain. This is a product security architecture
   decision, outside the camera provider bootstrap.
2. **Upstream advertisement.** Even with a source, section 2 still applies.

**Contract to apply when a source exists (guidance only, not implemented):**

* Events are formed and committed under the runtime state mutex.
* Delivery runs on the existing Health worker, outside every provider lock.
* Delivery is FIFO by a manager-lifetime commit sequence.
* The queue is fixed-capacity. Overflow drops the oldest record, is counted, and
  is reported in diagnostics.
* Nothing is replayed to a later registration unless explicitly retained.
* Disable and detach drain admitted callbacks; nothing is delivered after
  detach.

## 5. What changes

**Production behavior: none.** No file under `src/`, `include/` or
`third_party/` changes. There is no new capability, factory, ABI or Health
cadence.

Tests:

* `tests/provider_required_metadata_tests.inc`, run in `provider_contract`, has
  five tests:
  * **contract:** both exact overloads return `NotSupported`; the capability set is
    unchanged; the supported registration contract and S/D/B/U ordering are
    unchanged.
  * **states:** initial, configuration failure/recovery, running, faulted and
    recovered, with mutual MFA_Status/Detailed/Subsystem consistency checked in
    each.
  * **concurrency:** three registering/querying threads run alongside Image
    delivery and Health publication.
  * **re-entry:** same- and cross-provider callback context, with contained
    consumer exceptions.
  * **lifetime:** disable, detach, Control destruction, no late callbacks, and
    4096 rejected registration rounds with no retention.
* Compile-time tripwires:
  * the exact signature types;
  * no `DiscreteStatus` or security-audit enumerator exists (a positive control
    checks `SubsystemStatusResp`);
  * `Reserved3/5/9/10` remain 28–31.

  If a future pinned revision adds an enumerator, the build fails and this
  classification must be reevaluated.
* `tests/public_required_metadata_demo.hpp` is a published-MEL-only check, used
  by:
  * the native loader, both as `native_loader_required_metadata`
    (`--required-metadata-only`) and in its default run;
  * the installed consumer.

## 6. Prepared upstream issue (not filed by this task)

> **Title:** `ChannelMetadataCapabilityType` lacks enumerators for required Health
> callbacks `DiscreteStatus` and `MFA_SecurityAuditRecord`
>
> **Revisions:** ir-mel `8d9224519f12b44e0b28815755c56a32a28d24a0`, common-mel
> `f6908437d8fd2f7fb69896f9eb9cfd272d10c439`.
>
> `HealthStatusChannel.h` annotates the `DiscreteStatus` and
> `MFA_SecurityAuditRecord` `registerMetadataCallback` overloads as `@Required`.
> `ChannelMetadataCapabilityType.h` notes that each value corresponds to a
> `registerMetadataCallback`. `ChannelCapability.h` says that registrations
> without a corresponding advertised entry "are expected to return
> Return::NotSupported".
>
> The enumeration has no entry for either payload, so a provider cannot both
> implement them and report its capabilities consistently. `Reserved3/5/9/10`
> have no assigned meaning.
>
> **Request:** append explicit enumerators (for example `DiscreteStatus` and
> `MFASecurityAuditRecord`) without renumbering existing values, or document the
> intended advertisement rule for these callbacks. The optional `NUC_TempData`,
> `CalibrationConfiguration` and `CalibrationStatus` callbacks have the same gap.

## 7. Health status after this task

| Health payload | Status |
| --- | --- |
| `MFA_Status`, `MFA_StatusDetailed`, `BIT_Status`, `SubsystemStatusResp` | Partial software-provider profiles (unchanged) |
| `DiscreteStatus` | **Unresolved:** blocked by the upstream capability enum; no discrete source beyond existing reports |
| `MFA_SecurityAuditRecord` | **Unresolved:** blocked by the upstream capability enum **and** a missing provider security event source |
| `LFStatus` | Required only with local functions; none exist (`NotSupported`) |
| `NUC_TempData` | Optional; no sensor source (`NotSupported`) |

`HealthStatusChannel` remains **partial**. The provider is not MEL-conformant.
