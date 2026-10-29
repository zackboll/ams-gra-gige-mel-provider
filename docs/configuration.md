# Versioned camera configuration and C2 command status

This is a **provider-defined** JSON schema, not a standard MEL JSON schema.
It implements the pinned C2 `ConfigSetCommand` without changing upstream
declarations or adding a public ABI. The supported profile remains staring
Mono8, one band. No SDK or physical camera has been qualified.

## Schema version 1

A complete working payload is:

```json
{
  "schema": "gige-mel.camera-config",
  "version": 1,
  "width": 64,
  "height": 48,
  "offset_x": 0,
  "offset_y": 0,
  "frame_rate_hz": 30,
  "exposure_ns": 1000000
}
```

All eight members are required, in any order. The six settings are a complete
snapshot, never a patch. Width/height are positive pixel counts, offsets are
nonnegative sensor pixel coordinates, rate is positive integer hertz, and
exposure is positive integer nanoseconds. Dimensions, offsets and rate must
fit `uint32_t`; exposure must fit `chrono::nanoseconds::rep`. Bounds are checked
before narrowing or duration construction. Zero never means “leave unchanged.”

Input is limited to **4096 bytes including whitespace**. Exactly one flat JSON
object is required, with complete-input consumption and ordinary surrounding
JSON whitespace allowed. Missing, unknown and duplicate **decoded** names
(including `width` and `\u0077idth`) are rejected. Arrays/nested objects,
floating/exponent number tokens, numeric strings, booleans, null, comments,
trailing commas/content, embedded NUL bytes and malformed string encodings are
rejected. The parser validates string encoding. An integer version other than
1 or another schema identifier is unsupported; structurally invalid documents
are invalid parameters even if they also name an unsupported version.

Version 1 does not select a backend, device, format, trigger, calibration or
navigation source. Future incompatible schemas require a new version and an
explicit implementation/profile update. Camera-specific ROI, increments,
rate and exposure constraints remain in the common layout/backend validation;
the parser does not impose the synthetic camera's 640×480 or 1–120 Hz limits
on future adapters.

`commandID` is echoed exactly, including zero and `UINT32_MAX`. `systemTime` is
caller context, as for CameraCommand: no scheduled execution or clock epoch is
claimed. There is no command update/delete protocol, persistent command history,
or duplicate-ID cache. Use distinct IDs for requests that must be distinguished.

## Application and futures

ConfigSet and CameraCommand share one callback-free typed camera transaction,
serialized through the runtime operation mutex. C2 need not be enabled.
Image acquisition must be explicitly disabled, including after a stream fault
that retains acquisition ownership. Input/schema rejection does no backend apply
or readback and leaves configuration and its generation unchanged.

After every apply that returns, even an error, the provider queries actual
configuration and advances its generation. Valid actual readback is retained
after partial failure. Failed or layout-invalid readback invalidates configuration
and prevents enable, even with large buffers. Success requires successful apply
and exact valid actual readback. **Rejected does not mean the device is unchanged.**
Backend adapters report ordinary failures through their existing Result values.

| ConfigSet outcome | Ready future contains | Terminal status / reason |
| --- | --- | --- |
| Successful apply and exact valid actual readback | `Return::Success` | Accepted / NotSet, empty description |
| Representation/schema error | `ErrorCode::InvalidParameters` | Rejected / InvalidInputParameter |
| Unsupported schema identifier or integer version | `ErrorCode::Unsupported` | Rejected / CapabilityUnavailable |
| Image active, including faulted but not disabled | `ErrorCode::InvalidState` | Rejected / StateOrSettings |
| Common layout unsatisfiable, backend apply invalid argument/unsupported | `Return::Fail` | Rejected / InvalidInputParameter or CapabilityUnavailable |
| Apply success but readback mismatch/layout violation | `Return::Fail` | Rejected / Other |
| Backend transport/state/resource or readback failure | Existing mapped MEL error | Rejected / mapping below |
| Callback reentry, detached channel, closing Control | `ErrorCode::InvalidState` | No new status callback |

All rejected statuses contain a useful description from that particular outcome,
not mutable diagnostic history. Backend errors map explicitly as follows:

| Backend category | MEL error (when error-only) | CannotComply |
| --- | --- | --- |
| invalid_argument | InvalidParameters | InvalidInputParameter |
| unsupported | Unsupported | CapabilityUnavailable |
| invalid_state, busy, stopped, **timeout** | InvalidState | StateOrSettings |
| not_found | InvalidId | UnknownID |
| resource_exhausted | InsufficientLocalResources | InsufficientResources |
| disconnected | InsufficientRemoteResources | SubsystemUnavailable |
| io_error | InsufficientRemoteResources | SubsystemFault |

MEL has no Timeout error code; synchronous timeout uses StateOrSettings with
the backend description. A mismatch without a more precise failure uses Other.
An apply invalid_argument/unsupported remains an ordinary negative response;
a readback failure takes precedence and is error-only.

CameraCommand preserves its response conventions: `commandSuccess=true` maps
to Accepted, false to Rejected, and error-only futures do not fabricate an Image
response. Response timestamp is host steady-clock response time, size stays zero.

Disable, finish reading/release outstanding buffers, submit a snapshot, inspect
the future and current Image capabilities, resize/re-register buffers if needed,
then enable explicitly. Configuration never releases or overwrites held bytes.
Enable requires every registered buffer released and large enough. No automatic
restart or live reallocation is added.

## Status scope and callback contract

C2 discovery and per-channel capabilities advertise CommandStatus independently
of registration. Only **C2 ConfigSet and C2 CameraCommand** emit it. Image-origin
CameraCommand emits its existing Image response, not status on an arbitrary C2.
ConfigSet never emits Image CameraCommandResp. Communications/keep-alive retain
their reply mechanisms; other unsupported commands remain unsupported.

For ordinary attached use with a registered callback there is one terminal
Accepted/Rejected event per supported request. No Received event, queue or
worker is added. In this synchronous profile acceptance is decided after apply
and readback; this does not redefine Accepted universally as execution completed.

C2 CameraCommand delivers Image response metadata first (when there is a
completed response object), then C2 status. Each dispatch follows its channel's
admission rules. Futures are prepared before consumer code runs. Callbacks run
on the submitting thread outside operation/state locks; exceptions are contained,
increment callback-failure diagnostics, and cannot alter the determined result.
Strong Control/channel references survive result preparation and dispatch.

The first valid registration succeeds, empty returns BadPointer, duplicate returns
Fail, and registration from any provider callback or on a detached channel fails.
Mutation from **any provider callback, including another provider instance**, is
rejected before parsing or operation locking. Nested rejected configuration sends
emit no status, preventing recursive rejection callbacks. Ordinary commands work
again after callbacks return or throw.

Detach closes callback admission and drains already admitted callbacks. A status
not yet admitted may be suppressed by concurrent detach. No callback admission is
reserved before acquiring the operation mutex. **The request future is authoritative
across detach races; unconditional status delivery is not promised.**

## Private parser and offline verification

nlohmann/json **3.12.0**, immutable commit
`55f93686c01528224f448c19128836e7df245f72`, is vendored unmodified from
[`nlohmann/json`](https://github.com/nlohmann/json/tree/55f93686c01528224f448c19128836e7df245f72).
The release's annotated tag resolves to that commit (unsigned; no signature claim).
The single header and MIT license were compared with immutable Git tree blob IDs
before recording SHA-256, blob SHA-1 and size in `third_party/json-manifest.json`.
The original 97-file MEL manifest and bytes are unchanged.

```sh
python3 scripts/vendor_upstream.py --verify
python3 scripts/verify_json.py
```

Normal builds run both checks without network access. A small private SAX handler
validates decoded names and numeric event types, aborts on invalid events and uses
strict complete-input parsing, rather than DOM discard callbacks. Embedded NUL is
checked explicitly. Parser types/headers are confined to implementation sources;
no JSON header or package is required by installed consumers. The MIT license is
installed with distribution notices. The separate installed-consumer example
demonstrates ConfigSet and correlated status using only published MEL headers.