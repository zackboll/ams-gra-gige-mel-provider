# GIGE-MEL-GENTL-003A — Isolated read-only wire qualification

Incorporated main: `2a8a386553ecdfd7a89d95bec3f8c30843c5b2a1` (merged PR #43).
See [executed validation](GIGE-MEL-GENTL-003A-validation.md).

## Boundaries and foundation audit

Reviewed repository instructions, README, implementation profile, architecture's
adapter/ownership/shutdown boundaries, `src/gentl/`, `src/backends/gentl/`, probe,
existing qualification supervisor, Aravis loopback fixture, controlled-GVCP tests
and codec, single-owner design, and GENTL-003 design/validation.

The consumer, backend, GenApi conversions, quarantine/ownership policy, profiles,
factories, selection, timestamp behavior, production Aravis/Pleora adapters and
MEL capabilities are unchanged. There is no acquisition or navigation work.
All scripts are uninstalled and explicitly launched. The optional probe remains
default-OFF. Ordinary tests require no SDK, network namespace privilege or camera.

Reuse, rather than a second transport:

* `gentl_qualification.py` remains the entry point and bounded outer supervisor;
  its default offline load/interface mode retains GENTL-003 semantics.
* `gentl_probe.cpp wire-readonly` reuses the existing consumer's exact-ID methods
  and READONLY access. It requires a singleton discovered device on the explicit
  interface, refuses ambiguity, and never requests control or retries a failure.
* The camera uses **the same pinned Aravis 0.8.36 ArvGvFakeCamera** as
  `tests/support/aravis_gv_loopback.*`, through public C functions loaded in a
  dedicated Python ctypes child. This avoids the fixture's production-backend
  dependency and socket audits that assume same-process ownership. No new GVCP
  responder or transaction engine was added. The 038 transport's ACK codec is
  client-correlated, not a passive command decoder; its wire constants inform the
  independent observation-only parser, with literal-packet regression checks.

## Isolation and lifetime

Explicit wire launch uses:

```text
unshare --user --map-root-user --net --pid --fork --mount-proc --kill-child --
  python3 .../scripts/gentl_qualification.py --wire-worker EXTERNAL/config.json
```

Namespace PID 1 checks that network and user namespace identities differ from
the parent, only a DOWN `lo` exists, and no routes or unexpected addresses exist.
Only then does it bring **that new namespace's lo** UP. It rechecks addresses,
interfaces and all IPv4/IPv6 route tables. Only 127.0.0.1/8, ::1/128 and local
loopback routes are used. No host NIC is moved or visible; no host interface,
address or route is changed. Physical NICs, veths, external/default routes,
same-as-parent namespaces and preconfigured namespaces are rejected. Full before
and configured `ip -j` output and namespace identities are retained.

This is a deliberately configured **simulated network**, not changing Balluff's
test environment to work around the GENTL-003 crash. Balluff 3.7.2.4495 crashed
at TLUpdateInterfaceList with loopback DOWN. That finding remains unresolved;
no automatic retry, modified-environment Balluff run, licensing change or Cognex
contact is performed here. A separately authorized offline diagnostic would need
its own documented environment change and evidence.

The worker owns the camera, observer descriptor and producer child. It polls
producer/camera liveness, wire violations and the deadline every 5 ms (not a
real-time scheduling guarantee). Camera startup has 5 seconds; IP commands have
2 seconds; calibration has 1 second; producer sequence has `--timeout` (0.1–300
seconds, at least quiet+1); native finalization has 5 seconds; quiet is 0.1–10
seconds. A whole-sequence deadline also bounds every individual native stage,
including dlopen/dlclose, even if a producer ignores its GenTL timeout. The outer
supervisor bounds the namespace at timeout+20 seconds. No uncertain native call
is retried. Any write/unqualified packet aborts the producer sequence immediately
on observation; no further device stage is requested.

On all exits PID 1 kills remaining namespace processes and reaps all children,
including adopted vendor descendants. Unexpected descendants prevent a pass.
The outer process-group kill and unshare kill-child retire the PID namespace on
crash/hang; namespace-init exit causes Linux to kill remaining members. Killed
processes do **not** establish native cleanup. Core dumps are disabled. Child
files are capped at 64 MiB; trace records at 10,000; packet-drain batches at 4096.
Raw evidence remains outside Git in a new exclusive output directory.

This is crash/network containment for **trusted** producer binaries, not a
hostile-code sandbox: filesystem/devices are not isolated and a malicious
producer with inherited host resources is outside the trust model. The launcher
does not pass network descriptors. No elevated host installation is needed.

## Simulator and independent observer

The native camera binds 127.0.0.1:3956 and its upstream global-discovery listener
inside the namespace. That wildcard is safe only because there is no host NIC
in this network namespace, not because an Aravis discovery-interface option
confines the simulator. The embedded upstream XML is controlled/pinned and saved
with a hash (15,983 bytes locally); identification, discovery, register/memory
reads, TestRegister and CCP state are available. Initial/final CCP, TestRegister
and acquisition registers are recorded. Shutdown calls `g_object_unref` (joins
the native simulator thread), then `arv_shutdown`; the parent checks exit 0,
shutdown evidence and that the GVCP port can be rebound.

Aravis does not know the client's GenTL access request. It can accept an illicit
write from an unowned endpoint. **This is not accepted as read-only behavior**:
the independent observer records every command before acceptance/rejection, and
all writes fail qualification regardless of ACK or final register value. The
injected CCP test actually changes simulator CCP to 2 and fails; it is not hidden
by a simulator that always returns success or always rejects writes. Rejected
and unknown commands remain in the packet trace. No simulator enters the provider.

`gentl_wire.Observer` opens Linux `AF_PACKET/SOCK_DGRAM/ETH_P_ALL` on `lo`, separate
from producer reporting and simulator logic. It receives IP datagrams with
kernel `SO_TIMESTAMPNS_NEW` timestamps; it neither intercepts producer API calls
nor relies on producer-declared access. On loopback only incoming packet copies
are retained (PACKET_OUTGOING is omitted by kernel packet type, **not** by payload
deduplication). Real repeated datagrams remain visible. Missing timestamps,
truncation, fragments, non-IPv4/non-UDP/unexpected-port traffic are unqualified.
Unsupported IPv6 is not silently treated as safe.

Before producer launch, a real discovery+ACK calibration must appear exactly once
in the observer. It is retained as a distinct calibration phase. The observer
stays active through native close, process exit, the additional post-exit quiet
interval, and simulator shutdown. It reads PACKET_STATISTICS once, at the end;
any drop or missing/incomplete capture prevents a pass. There are no unchecked
packet-structure casts. Python length checks precede unpacking, with aligned,
bounded, nonwrapping address intervals. Full IP and GVCP bytes are retained.

| Command | Classification |
| --- | --- |
| 0x0002 discovery, empty payload | Discovery |
| 0x0080 READREG, one or more aligned addresses | ReadRegister |
| 0x0084 READMEM, aligned bounded address/size | ReadMemory |
| 0x0082 WRITEREG, address/value pairs | WriteRegister |
| 0x0086 WRITEMEM, bounded interval | WriteMemory |
| Any write covering 0x0a00 | ControlPrivilegeModification |
| Unknown ID, malformed header/flags/length/ID/payload | Unqualified |

Commands and ACKs must correlate by client endpoint, transaction ID and command;
ACK status, length and memory address echo must agree. Incomplete exchanges
cannot pass. Repeated transaction IDs from the same endpoint (including payload
changes) are unqualified duplicate/retransmitted behavior, not silently retried
safe reads. This deliberately conservative policy may reject legitimate producer
retries; such behavior must be separately qualified, not implicitly allowed.

## Probe sequence and results

The new probe command loads the one explicit producer, prints GCGetInfo version
facts, initializes/opens the system, updates interfaces, selects the exact listed
interface, discovers exactly one device, and opens **that discovered ID** with
`DEVICE_ACCESS_READONLY`. It inspects module/port information, attempts supported
Local XML reads using the existing bounded XML-head diagnostic and a raw selected
TestRegister feature read. It does not introduce a GenApi XML loader or change
feature conversion. Unsupported port/XML information is logged, not emulated;
ZIP/File/http remain unsupported by existing semantics.

It then checks DevClose, IFClose, TLClose, GCCloseLib and producer destruction,
in that order, with timestamped stage/close markers. The child remains alive
without producer objects for the quiet interval, so process exit cannot conceal
producer background traffic. The parent additionally watches after exit for
surviving helpers. A failed native operation uses subprocess-only `_Exit(1)`;
there is no destructor retry or claim that OS process retirement was native
cleanup. Existing general consumer/backend cleanup contracts are unchanged.

The version-2 extension of `gige-mel.gentl-offline-qualification` retains command
lines, native statuses/signals/timeouts, binary SHA-256s, reported producer version
properties, simulator version/XML, network details, requested access, timestamped
packets/lifecycle markers, written addresses, CCP writes, command counts,
unmatched transactions, violations, native shutdown diagnostics and hashes/sizes
of evidence files. `result.json` hashes the other evidence, not itself. Tool-source
hashes identify the decoder/runner. Logs may contain sensitive local paths.

Terminal results:

* `READ_ONLY_WIRE_PASS`: actual selected producer executed the whole sequence,
  complete calibrated drop-free independent wire evidence, discovery and reads,
  no writes/unknowns/retransmits/post-close commands, clean native shutdown and
  no unresolved descendants. READONLY alone is insufficient.
* `READ_ONLY_WIRE_VIOLATION`: configuration/CCP/memory write, malformed/unknown
  packet, duplicate transaction or any command after successful device close.
* `PRODUCER_CRASH`, `PRODUCER_TIMEOUT`: process status, never successful cleanup.
* `NATIVE_CLEANUP_UNRESOLVED`: failed/missing close markers, failed shutdown,
  unexpected descendants or failed enclosing supervisor.
* `WIRE_OBSERVATION_UNAVAILABLE`: unavailable/uncalibrated/dropped/incomplete
  capture, unmatched exchanges, absent actual discovery/reads or isolation failure.
* `PRODUCER_NOT_AVAILABLE`: explicit missing/no producer; nothing is loaded.
* `HARNESS_SELF_TEST_PASS`: SDK-free synthetic client only. **Never** third-party
  producer or physical camera qualification. Other synthetic cases must fail.

Crash/timeout takes precedence over simultaneous wire findings; all findings are
still retained. No failure produces a safe-read-only conclusion.

## Explicit use (all paths absolute)

No proprietary producer runs in ordinary CI. Manual SDK-free integration:

```sh
python3 /home/zboll/git/ams-gra-gige-mel2/tests/gentl_wire_live_tests.py \
  --aravis-library /home/zboll/sdk/aravis-0.8.36/lib/libaravis-0.8.so \
  --output /home/zboll/gentl-003a-evidence/new-live-matrix
```

For a future authenticated Euresys installation, explicitly supply its absolute
CTI path, the built probe, and its documented exact loopback interface ID:

```text
python3 ABS_REPO/scripts/gentl_qualification.py --wire
  --aravis-library ABS_ARAVIS/lib/libaravis-0.8.so
  --probe ABS_BUILD/gige_mel_gentl_probe --producer ABS_VENDOR/producer.cti
  --interface EXACT_ISOLATED_INTERFACE_ID --timeout 20 --quiet 1
  --output ABS_EXTERNAL_NEW_EVIDENCE_DIRECTORY
```

No host interface name, automatic producer fallback, interface-info workaround,
environment retry or physical destination option exists in this mode. Some
producers may not support loopback discovery; failure is an unqualified producer
limitation, not justification to attach a physical network. Producer-specific
virtual-Ethernet support would require a separately audited isolation extension.
GENTL-004 acquisition remains separately gated and is not begun.