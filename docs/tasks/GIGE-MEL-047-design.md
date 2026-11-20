# GIGE-MEL-047 — Camera-profile navigation handler interface

**Capability disposition: `PROFILE_NAVIGATION_FIXTURE_QUALIFIED`.** A camera
profile can now supply its own navigation policy and prepare an owned operation,
and a profile-selected, SDK-free recording backend exercises it through the
unchanged MEL `ImageChannel::send(NavigationReport)`. Nothing is transmitted.
This is not `PRODUCTION_CONTROL_QUALIFIED`, `NAVIGATION_TRANSPORT_READY` or
physical detector navigation processing. Executed evidence is in the
[validation record](GIGE-MEL-047-validation.md).

## Baseline audit

Baseline: freshly fetched `origin/main` at `076548635ed664434a9746a22b82c9aad95f7e3b`
(merge of PR #59, GIGE-MEL-046). These existing contracts were confirmed in
source and are unchanged:

| # | Contract | Source observation |
| --- | --- | --- |
| 1 | MEL validation first | [`provider.cpp`](../../src/provider.cpp) `ImageChannelImpl::send(NavigationReport)` runs `navigation_error` (finite values, ranges, variances, state) and the strictly increasing `systemTime` check under the operation lock, before any backend call. |
| 2 | Sink discovery | The provider uses `dynamic_cast<backend::INavigationSink*>` on its opened `ICameraDevice`. |
| 3 | Receipt-only without a sink | No sink: exact echo, commit and callback, as before. |
| 4 | Sink success is backend acceptance | [`backend.hpp`](../../include/gige_mel/backend.hpp) `INavigationSink::submit_navigation`: success means backend acceptance only. |
| 5 | Uncertainty is not rejection | `timeout` maps to MEL `InvalidState` "acceptance is uncertain"; a sink exception is reported as uncertain, not as a definite rejection. |
| 6 | State independent of parent profile | `ICameraProfileState` owns its dependencies (tested in `device_feature_tests`). |
| 7 | Loader wraps state | [`profile_loader.cpp`](../../src/selection/profile_loader.cpp) returned a private `final State` wrapper; a cross-cast on the host-visible object could not find a module companion interface. |
| 8 | No production grant | [`authority.hpp`](../../src/control/authority.hpp) admits an executor only through `test::FixtureAdmission`. |

`src/provider.cpp`, `include/gige_mel/*`, `camera_profile.hpp`, `profile_module.hpp`,
the GIGE-MEL-046 authority and the Cognex module are byte-for-byte unchanged.

## Type model

[`navigation_handler.hpp`](../../src/profiles/navigation_handler.hpp) (new; in the
module identity closure):

```cpp
struct NavigationOperation {          // plain owned value
    std::string format;               // nonempty, profile-defined layout label
    NavigationReport report;          // complete, unchanged copy
    std::vector<std::byte> payload;   // profile-defined bytes
};
class IProfileNavigationHandler {     // optional companion of ICameraProfileState
public:
    virtual Result<NavigationOperation> prepare_navigation(const NavigationReport&) const = 0;
};
const IProfileNavigationHandler* navigation_handler(const ICameraProfileState&) noexcept;
```

* **Optional and discoverable.** A concrete state inherits both interfaces, and
  `navigation_handler()` cross-casts. Absence means no navigation processing.
  No default or fabricated handler exists. The `ICameraProfile` and
  `ICameraProfileState` vtables and descriptor v1 are unchanged (verified).
* **Preparation separated from execution.** The handler is bounded, makes no
  device I/O, retains no reference and starts no work. It returns either an owned
  value or a definite error (`unsupported`, `invalid_argument`, ...).
* **Ownership.** `NavigationOperation` holds only standard-library values, with
  no virtual functions, deleters or handles. It may outlive the state and the
  module that produced it, and its last owner (normally the backend port)
  destroys it. The profile author controls the payload layout; there is no
  universal detector wire format.
* **No implicit conversion.** `report` must equal the submitted report exactly;
  the bridge refuses an operation whose copy differs.

## Bridge and backend port

[`navigation_bridge.hpp`](../../src/profiles/navigation_bridge.hpp) defines
`INavigationOperationPort`, a backend-owned boundary that receives an owned
operation and returns `NavigationExecution` evidence (`not_submitted`,
`accepted` or `uncertain`, plus control loss). `ProfileNavigationBridge` provides:

* `create(state, port)`: refuses with `unsupported` when the state has no handler,
  in which case the device should expose no `INavigationSink`. It holds a `shared_ptr`
  to the state, so the handler and any module code behind it outlive every
  admitted submission.
* `submit(report)`: synchronous, suitable for `INavigationSink::submit_navigation`.
  One active submission at a time; no queue or worker.
* `shutdown()`: closes admission, waits for the active submission, then
  releases the port and then the state *outside* the lock. Reentrant shutdown from
  the handler or port returns false and changes nothing. The destructor calls it.

Mapping to the existing `backend::Status`:

| Stage outcome | Status | MEL result (unchanged provider mapping) | Port invoked |
| --- | --- | --- | --- |
| Port `accepted` | success | ready response + callback | yes |
| Handler `unsupported` | `unsupported` | `Unsupported` | no |
| Handler `invalid_argument` / other Error | that code | per existing mapping | no |
| Handler exception | `invalid_state` (`resource_exhausted` for allocation) | `InvalidState` / `InsufficientLocalResources` | no |
| Empty format or altered report | `invalid_state` | `InvalidState` | no |
| Port `not_submitted` | port code (`disconnected` on loss) | per existing mapping | yes |
| Port `uncertain`, port exception, acceptance with/after loss | `timeout` | `InvalidState` "uncertain" | yes |
| Later submission after uncertainty | `invalid_state` "unresolved" | `InvalidState` | no |
| Later submission after loss, or retired | `disconnected` | `InsufficientRemoteResources` | no |
| Concurrent / reentrant submission | `busy` / `invalid_state` | `InvalidState` | no |

Preparation failures are definite, because the port has not been entered.
After entry, absence of proof means uncertainty. Uncertainty is sticky: there
is no automatic retry, and later success cannot mask an unresolved effect.
Acceptance reported together with control loss is not trusted.

## Stages kept distinct

| Stage | Claim established here |
| --- | --- |
| MEL validation | Input structurally acceptable (unchanged provider) |
| Profile preparation | Sensor-specific policy produced an owned operation; **not** success by itself |
| Backend acceptance | The recording port accepted it (fixture) |
| Transport acknowledgement | Not implemented; needs a qualified transport |
| Detector acceptance | Not implemented; needs a detector-defined protocol |
| Image association / transfer alignment | Not implemented |

## Dynamic modules

`profile_loader.cpp` now builds one of two wrappers after the module's `open`:

* `State` (unchanged behavior) when the module state does not implement the handler.
* `NavigationState final : State, IProfileNavigationHandler` only when
  `navigation_handler(*module_state)` is non-null. It forwards through a
  reference to the module state's own handler subobject. That subobject lives
  exactly as long as `State::state_`, which is destroyed before the module pin
  (`module_` is declared first). No extra ownership, worker or feature service is
  retained, and the existing destruction order is unchanged.

The capability therefore survives wrapping, and the wrapper never fabricates it.
The descriptor, discovery symbol, `interface_version` (1) and base virtual order
are unchanged. `navigation_handler.hpp` is added to `ProfileBuildIdentity.cmake`,
so the identity string changes. **Old module binaries are not compatible with
this host.** The loader refuses them as "incompatible C++ build identity" before
the factory runs. Rebuild host and modules together; because the identity hashes
raw bytes, comment-only edits to that header also require a rebuild.

## Example profile and backend (test only)

[`navigation_record_policy.hpp`](../../examples/profile_module/navigation_record_policy.hpp)
defines `test_navigation_record_v1` (Profile B). Camera behavior is delegated
to the shared monochrome representation. Its handler:

* rejects `Aligning` as `unsupported` and position variances above a fixture
  limit as `invalid_argument`;
* builds a **fictional test record** (`TESTONLY` magic, deterministic test field
  IDs, little-endian TLV with IEEE-754 bit patterns, fixture state codes);
* keeps the complete report alongside it, with no conversion.

It contains no register addresses and is not an AMS-GRA, GigE Vision or GVCP
format. It is compiled statically into the test, and separately as
`libnavigation_record_profile.so` via [`navigation_module.cpp`](../../examples/profile_module/navigation_module.cpp).

The test-only [generic backend](../../tests/support/profile_navigation_backend.hpp)
opens any selected profile on a static feature fixture and delegates imaging to
the synthetic backend. It exposes `INavigationSink` **only** when
`navigation_handler()` is non-null, and its port records operations. Profile A
(built-in `mono_free_running_v1` and test `test_rate_limited_v1`) therefore keeps
receipt-only behavior; there is no sink that returns unsupported.

## Threading and shutdown

* Provider serialization is unchanged: `submit_navigation` runs under the provider
  operation lock and the existing reentry guard. The bridge mutex covers only
  admission bookkeeping, never handler or port calls.
* Handler and port must not call provider or consumer code. Same-thread bridge
  reentry is refused; provider reentry is refused by the existing guard.
* Frame receive may overlap submission. The example handler uses immutable
  state and atomics only. The `validate_frame` concurrency rules are unchanged.
* `device.close()` and device destruction call `shutdown()` first. Retirement
  runs outside locks after the drain. The last module pin is released only
  after the port and the bridge's state reference are released.

## No production enablement

No production Aravis, Pleora or GenTL device implements this sink or links
`gige_mel_profile_navigation`. There is no authority grant, GVCP socket,
register/memory write, or GenICam feature, acquisition, heartbeat,
stream-destination or timestamp change. The Cognex profile is unchanged and has
no handler. Physical navigation remains blocked until there are qualified
control ownership, bounded transport completion, cross-session fencing and a
detector-defined staging/acceptance protocol
([GIGE-MEL-046](GIGE-MEL-046-design.md#future-navigation-boundary-not-implemented)).
