# Backend-independent NavigationReport forwarding (GIGE-MEL-023)

## Implemented interface

The existing MEL ImageChannel NavigationReport entry point optionally forwards
validated navigation reports to a backend navigation sink. The MEL declarations,
factory signatures, capability advertisement and response layout are unchanged.
`include/gige_mel/navigation.hpp` defines project-owned values without MEL or
backend implementation dependencies. `backend.hpp` defines `INavigationSink`,
an optional C++ source interface implemented alongside `ICameraDevice`; it is
not a stable binary plugin interface.

Devices without that interface retain the existing [receipt-only contract](navigation-reception.md).
The default synthetic backend does not implement a sink. An implemented sink's
`unsupported` result is returned as MEL `Unsupported`, not silently downgraded
to successful receipt. No production backend navigation transport is provided.

## Data and acceptance

Conversion preserves every validated input field without normalization or unit
conversion: systemTime; all four accepted navigation solution states (Aligning,
FreeInertial, GPS, Blended); latitude, longitude, altitude and altitudeMSL;
attitude; attitude rate and its separate timestamp; north/east/down speed and
acceleration; wander angle; magnetic heading; and all 18 named position and
velocity covariance values, including signed cross terms. The existing finite,
range, variance, state and timestamp validation rules remain in force. Invalid
and stale reports never reach the sink. Nanosecond timestamps retain their exact
values and unspecified source epoch.

`submit_navigation(NavigationReport)` receives a value and returns `Status`.
Success means explicit **backend acceptance**. The provider then retains the
report and delivers the existing ready response and optional synchronous Image
callback. Response time remains the exact input echo, with zero/unassigned IDs.
The response does not distinguish receipt-only operation from backend acceptance;
the selected backend implementation determines the behavior.

There are four distinct boundaries: MEL receipt, backend submission acceptance,
physical transport delivery, and downstream navigation processing. Only the
first two are implemented or asserted here. Navigation solution state is copied
as a label distinct from numeric accuracy or covariance; no quality is inferred.

The callback snapshot and response allocation precede submission. Backend
failure leaves the attachment's latest accepted report and monotonic admission
threshold unchanged and emits no callback. A later explicit submission may retry
the same timestamp. No automatic retries or navigation queue are added.

| Backend failure | MEL result |
| --- | --- |
| unsupported | Unsupported |
| invalid_argument | InvalidParameters |
| not_found | InvalidId |
| invalid_state, busy, timeout, stopped | InvalidState |
| resource_exhausted or allocation exception | InsufficientLocalResources |
| disconnected, io_error or other exception | InsufficientRemoteResources |

Error results are ready futures. As with existing requests, constructing an
error future itself requires memory. An exception or failure cannot guarantee
rollback of a backend's external side effects. Sink implementations must define
their own failure semantics; callers must not infer exactly-once processing.
Timeout is explicitly an **uncertain acceptance outcome**, not definitive
rejection or success, and its MEL InvalidState diagnostic says so. A sink must
use timeout for other uncertain outcomes and explain them in diagnostic text.
Unexpected exceptions likewise report uncertain acceptance. The provider leaves
its local retained state unchanged; this does not establish external rollback.
Retry after an uncertain outcome may duplicate external effects and is solely
the caller's decision. Definitive backend errors assert non-acceptance.

## Threading and lifetime

Submission runs synchronously on the sending thread under the existing runtime
operation lock, serialized with navigation admission, configuration, lifecycle
and detach. It may overlap frame receive. A sink must bound its work, must not
call provider or consumer code, and must own any data retained after return.
A slow sink delays competing runtime operations; no separate cancellation or
asynchronous submission interface is implemented.
Each sink implementation must document a finite completion bound for every
call, including failures. The recording fixture's gate has a five-second
watchdog and otherwise performs only in-memory work. No transport deadline is
implemented by the provider. Same-thread sink mutation reentry is defensively
rejected using the existing callback restrictions; communications requests that
could invoke consumers are also rejected. A sink must never wait for another
thread to perform provider operations: that would deadlock behind the lock.

Control and Image remain pinned through submission and callback dispatch.
Callbacks are admitted before releasing the operation lock, run outside provider
locks, retain existing reentry rejection and exception containment, and are
drained by detach. Disable/enable preserves the attachment's timestamp rule;
reattachment starts a fresh receipt session without resetting backend state.
The provider does not replay retained reports to a backend or new callback.

## Tests and limitations

Three provider scenarios use a deterministic recording sink over the synthetic
backend. They check every converted field against independently specified
values, all four accepted solution states, signed covariance cross terms, large
and maximum nanosecond timestamps, invalid/stale rejection before submission,
receipt-only fallback, reattachment, retained frame bytes and empty frame
navigation/inertial fields. Failure tests cover every backend error category,
allocation and unexpected exceptions, no callback/commit on failure, explicit
retry, preparation failure before submission, and contained callback exceptions.
Gated submission and callback tests check acceptance ordering, concurrent stale
submission and detach drain. Retained-byte checks include a full buffer pool.
Additional checks prove increasing submissions can finish while an earlier
callback is held, sink mutation reentry is rejected, and dropping the last
external Control reference during submission cannot invalidate the operation.
The existing navigation, backend, lifecycle and public-loader contracts remain
enabled. The provider contract executable is already run by the existing CI
compiler and sanitizer matrices; configured CI is not evidence of execution.

Successful forwarding establishes neither navigation accuracy, clock
synchronization, image correlation nor sensor-side processing completion. Frame
navigation/inertial fields, timestamps, configuration generation and buffer
lifetime rules are unchanged. No navigation transformation or image association
is implemented. Future transport integration is an optional backend
implementation requiring a separately defined device protocol.

## Local validation

Executed sequentially in fresh build trees on Linux with GCC 14.2 and
Clang 19.1.7, based on main `138c2bf44d84cc8b6dbaac75de703cf80e053667`:

| Lane | SDK-free complete CTest | Optional adapter complete CTest |
| --- | --- | --- |
| GCC Debug | 9/9 passed | 16/16 passed |
| GCC Release | 9/9 passed | 16/16 passed |
| Clang Debug | 9/9 passed | 16/16 passed |
| ASan/UBSan | 9/9 passed | 16/16 passed |
| TSan | 9/9 passed | 16/16 passed |

The optional adapter tests use in-process and loopback fixtures; no physical
device qualification is asserted. All existing image, navigation, lifecycle,
C2, Health, BIT, Standby, format, stream and timestamp tests remain enabled.
Sanitizers used halt-on-error; ASan leak detection was enabled. No sanitizer
suppression was added or changed. Existing uninstrumented-dependency handoff
blind spots remain; these passes do not provide instrumented dependency coverage.

- Three navigation backend scenarios repeated five additional times in each
  SDK-free lane (25 repetitions of each scenario): all passed.
- Separately installed published-MEL consumer and package isolation: passed
  for all three ordinary compiler builds, including receipt-only navigation.
- Standalone installed backend/navigation header compilation with only the
  installed include path: passed with GCC and Clang.
- Pinned upstream verification: all 97 files unchanged; private parser integrity
  also passed. No public MEL declaration was modified.
- Documentation links, added-text terminology review and `git diff --check`:
  passed. Complete source, comments, tests and documentation reviewed.

Only deterministic synthetic/recording fixtures were used for navigation
submission; no external navigation source or device transport was tested.
Existing CI compiler/sanitizer jobs are unchanged. Hosted final-head results and
delivery identifiers are recorded on the PR rather than inferred from local
checks. Independent review remains a separate gate; no merge is authorized by
local qualification.
