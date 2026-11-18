# GIGE-MEL-GENTL-003 — Real producer qualification (design)

Branch: `feature/gige-mel-gentl-003-producer-qualification`. Incorporated `main`:
`9ded1b496c7e3b3811f78d57d3681c817256c555` (merged PR #42). See the
[executed validation record](GIGE-MEL-GENTL-003-validation.md).

## Scope and audit

Reviewed the repository instructions, README, implementation profile,
architecture's adapter/profile and ownership boundaries, `src/gentl/`,
`src/backends/gentl/`, `src/device_features.hpp`, `src/profiles/camera_profile.hpp`,
`camera_profiles/cognex_cic_5000r/`, probe, backend fixture example, both prior
GenTL designs/validation records, and historical Cognex record.

No consumer/backend ownership policy, production API, factory, catalog, profile
interface or admission policy changes. The diagnostic supervisor reuses the
existing consumer through its probe; it does not implement a second consumer.
No streaming, settings writes, control escalation, NavigationReport or MEL
integration is added. The Cognex module remains byte-for-byte unchanged.

## Safe stages and explicit stop

1. Research downloads and inspect existing external SDKs statically.
2. In a fresh, unprivileged user/network namespace, run offline producer loading
   and interface enumeration in bounded child processes. There is no physical
   NIC, route to a camera, or device discovery/open request. Loopback stays down;
   this legal environment is recorded rather than changed to accommodate a
   producer. Existing host camera/network configuration is untouched.
3. Only after successful initialization could an independent, isolated simulated
   camera establish read-only behavior. A consumer fixture's assertion that
   access flag 2 was passed is **not** proof of a vendor's wire behavior.
4. Physical discovery/open requires both the read-only safety evidence and
   explicit owner authorization for that producer. Earlier Aravis discovery
   authorization does not transfer. Neither gate is satisfied here.

On any error, crash, hang or uncertain cleanup, stop. No automated retry,
fallback, hidden `--no-interface-info`, loopback-up workaround, control access
or physical-camera attempt. The actual Balluff enumeration crashed; the sequence
therefore terminates before stage 3. A missing Euresys login does not prevent
completing the safe offline tooling, regressions and evidence.

## Offline diagnostic changes

`scripts/gentl_qualification.py` is an uninstalled Linux/Python 3.9+
standard-library supervisor. It accepts an explicit absolute existing probe and
producer file, a new absolute output directory **outside this checkout**, a
finite 0.1–300 second per-child bound (default 20), and 1–5 load runs (default 3).
There is intentionally **no device/discovery/open/acquisition/write option**.
Do not extend it to physical stages until a producer can pass the safety gates.

Every producer invocation uses `unshare --user --map-root-user --net --`.
A preliminary `true` checks namespace availability; failure stops before SDK
loading. This is Linux isolation and process-crash containment, **not a
security sandbox for hostile SDK code**: filesystem, USB and other host resources
are not isolated. Third-party binaries must still be trusted. No root install,
host namespace entry, network-link change or credential handling is performed.

Each child gets its own process group, no input, and stdout/stderr files rather
than potentially blocking pipes. Its working directory is the external evidence
directory (including any producer-relative artifacts or core dump if the host
enables them). Timeout kills the process group with SIGKILL; the supervisor
reaps the child and records timeout distinctly from its negative signal status.
No subsequent stage runs after failure. `result.json` schema
`gige-mel.gentl-offline-qualification` v1 records command arrays, elapsed/bounded
times, exit codes/signals, executable/producer SHA-256, selected dependency
environment paths, platform, and log sizes/hashes. Output directories/logs are
exclusive; earlier evidence is never overwritten. SDK credentials are neither
requested nor dumped. Logs and dependency paths can still be sensitive.

The probe's `--offline-qualification` flag accepts only `load` or `interfaces`
before any native call. It treats a failed `TLUpdateInterfaceList` as a failed
qualification, performs only orderly cleanup, and does not list interfaces
after that error. Existing diagnostic modes retain their semantics.

Flushed operation markers identify load/function-table/init, TLOpen, TLGetInfo,
TLUpdateInterfaceList, interface information, TLClose and GCCloseLib boundaries.
They are consumer observations, not independent vendor-internal tracing. If a
native close fails in this special **subprocess-only** mode, `_Exit(1)` preserves
the error without stack unwinding, destructor retry, parent native close or
dlclose. The OS retires the process; **successful native cleanup is not claimed**.
The general consumer's destructor contract is not changed. GENTL-002's registry,
quarantine and one-attempt cleanup contracts remain covered by existing tests.

A normal successful run reports the probe's close results; it explicitly does
not certify vendor-internal handle retirement, heartbeat cessation or actual
library unmapping. A failed run keeps `native_cleanup=NOT_ESTABLISHED`.
In particular a crash after TLOpen does not establish TLClose/GCCloseLib.

## Offline regressions

SDK-free supervisor tests cover success, nonzero exit, signal, bounded hang with
partial output, launch failure, namespace failure before SDK loading, immediate
stop at each stage, command allowlisting, output preservation and CLI bounds.
They run in ordinary CTest but never execute a real producer or namespace.
Probe fixture regressions cover forbidden offline commands before library load,
an interface-update native timeout with preserved error and no further
enumeration, and TLClose/GCCloseLib failures with exactly one native failure,
no destructor retry and no unload. Real-producer execution remains manual,
outside ordinary CTest and hosted CI.

## Subsequent qualification (not implemented or executed)

After resolving producer availability/behavior, independently capture the entire
read-only simulation sequence and a post-close quiet interval. Check for GVCP
WRITEREG/WRITEMEM, particularly control privilege at `0x0a00`, any access
escalation, ownership and heartbeat activity; retain packet evidence externally.
Do not infer these properties from an exported function table or access flags.

Only then request separate Cognex authorization, verify the currently selected
NIC/address/reachability and ownership without interrupting another controller,
and run exact-ID discovery/read-only inspection. Use the existing GenTL-backed
ArvDevice/Aravis 0.8.36 GenApi and unchanged `cognex_cic_5000r_v1` profile;
record feature reads even if admission rejects the power-on configuration.

Real XML URL form, lengths, SHA-1, byte order and register bytes are **unknown**.
Preserve GENTL-002's 16 MiB bound, well-formedness and hash checks. ZIP, `File:`
and `http:` remain unsupported. No need for ZIP has been established here. If
later required, propose a separate single-document ZIP task with validated
directory/entry agreement, no encryption/multidisk/path extraction, supported
compression only, bounded compressed and expanded lengths/ratio, CRC and
producer-hash verification, checked offsets/overlaps, and malicious-archive
regressions. Do not add a general archive reader speculatively.

GENTL-004 acquisition remains separately gated and must not use this incomplete
qualification as evidence that a producer/camera pair is safe.