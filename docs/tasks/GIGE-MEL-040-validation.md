# GIGE-MEL-040 validation and qualification record

**AUDIT_INCOMPLETE / NATIVE_RECEIVE_PENDING. Not READY_FOR_INDEPENDENT_REVIEW.**
The implemented investigation has packet-backed evidence of native Open GVCP
traffic, not a successful passive receiver qualification. See the
[audit/API/architecture record](../pleora-stream-only-audit.md).

## Source baseline and environment

Fetched main commit `26384976fc43cbc400e4e13c12f68f926c247e36`, tree
`30bc4842a50f832dc3085f9fe3424329cb72a147`; GitHub PR #33 MERGED at that commit.
Branch `feature/gige-mel-040-pleora-stream-traffic-audit`. Final commit/tree and
final-HEAD regression results are attached to the delivery PR rather than embedded
self-referentially here. Source changes remain test/tooling/documentation only;
production Pleora adapter and upstream snapshots are unchanged.

Host Linux x86-64 kernel 6.12.116, GCC 14.2, Clang 19.1.7, CMake/Ninja Debug.
External SDK `/home/zboll/sdk/pleora/ebus_sdk` resolves to the existing ibex mirror
Ubuntu-22.04-x86_64 prefix; eBUS 6.5.3.7155. Matching GenApi 3.4.1.1 headers:
`/home/zboll/sdk/gige-mel-027-genapi/headers/library/CPP/include`.
Aravis lanes use `/home/zboll/sdk/aravis-0.8.36`. No physical-camera tests enabled.
Namespace interface checks use if_nameindex, not host-mounted sysfs inventory.

An initial tcpdump capability probe (not qualification evidence) did not terminate
under timeout in its private loopback namespace; PID 1656010 remained after signal
attempts returned EPERM even on reentry to its user namespace. It has no physical
interface or device connection. Host-administrator cleanup is required if still
present. The implemented observer does not spawn tcpdump; it owns and explicitly
retires its packet socket/thread, with bounded native-child cleanup. No host route,
interface or camera setting was changed by that probe.

Native maps confirm actual eBUS/GenApi libraries from that resolved SDK. The four
loaded GenApi/logging SHA-256 identities independently match the approved audit:

| Runtime | SHA-256 |
| --- | --- |
| libGenApi_gcc48_v3_4.so | `9a2f6a5f2f6e85b22154ad467f75acafd1d1ec197122ea39e32d29df69ad0565` |
| libGCBase_gcc48_v3_4.so | `7534579ef0e4d513f569d97f26998c7378a322d2d58ea1d5d0d0ebced83604a4` |
| libLog_gcc48_v3_4.so | `935ded42d13cc143c73eb97a9b77f37ce4b4d6ae913370f148a33840b942a1a2` |
| liblog4cpp_gcc48_v3_4.so | `e053cc5f25f3ccea7aa573e6616565413cd1abc29e1b94fdd7f930830ce7544f` |

Maps and full loaded SDK hashes, executable hash, route/interface inventory,
pcap/packet JSONL, socket snapshots, responder records, native output and capture
coverage counters are private artifacts outside the repository under
`/tmp/gige040-evidence/`. They are not approved publication artifacts. No proprietary
SDK material or license keys are committed. No licensing or watermark alteration.

## Executed native modes (development evidence)

Independent unfiltered AF_PACKET capture starts before native process creation,
ends after native exit and 300 ms tail. All interfaces = lo, all destinations,
all protocols, native receiver plus fixture; zero kernel capture drops/errors.
Recorded Unix-epoch ns intervals below are bounded observations, not statements
about runtime beyond those intervals. Loopback packet counts include both copies.

| Mode / responder | PID | Capture interval ns (begin .. end) | Records | SDK GVCP source port | Native exit |
| --- | --- | --- | --- | --- | --- |
| default / absent | 2094812 | 1794714113373449468 .. 1794714115935941256 | 20 | 48479 | 2 |
| default / synthetic discovery ACK | 2096307 | 1794714116029122277 .. 1794714118595927087 | 16 | 43630 | 2 |
| requested disabled / absent | 2101176 | 1794714118683605623 .. 1794714121243919893 | 20 | 43347 | 2 |
| requested disabled / synthetic discovery ACK | 2103934 | 1794714121337854251 .. 1794714123900022863 | 16 | 52769 | 2 |
| unsuppressed TSan default / synthetic ACK | 2107008 | 1794714123982534443 .. 1794714127671946021 | 16 | 47872 | 66 |

Every command above: `127.0.0.1:source -> 127.0.0.2:3956`, DISCOVERY_CMD `0x0002`,
ID `0xb001`, one outgoing command during Open. ACK `0x0003` from fixture in present
cases, not an SDK command. All Open calls return exact code 25 NOT_FOUND; all Close
calls return code 23 NOT_CONNECTED. All completed stream object destruction.
Requested disabled modes never reached the setting, so no resend-disable success
is claimed. The executable hashes for these attempts are retained in coverage.json;
final-HEAD reruns belong in the delivery evidence, not inferred from these runs.

Socket barriers: native FD 3 persisted from runtime-initialized through
before-process-exit. Inodes respectively 19269433, 19341487, 19298209, 19254008,
19350178. No persistent native UDP endpoint in the barrier snapshots. The responder
exclusively owns its separate `127.0.0.2:3956` UDP socket. The packet observer owns
the AF_PACKET socket. An additional expanded inventory run retained fdinfo and
netlink/unix tables: native FD 3 inode did not appear in those namespace tables
either; it remains **unclassified**, not assumed to be non-GVCP. Transient native
Open command socket descriptor/inode is unknown, but its actual command endpoint
is captured. Barrier snapshots cannot prove absence of short-lived sockets.

No control privilege read/write, heartbeat, destination/configuration/acquisition
write, memory transfer or packet resend appears during these failed-open windows.
Other traffic: three SDK UDP syslog datagrams to loopback port 514 with responder,
four without it; kernel ICMP port unreachable responses. No unidentified captured
frames. No physical interface, external multicast or host egress route existed.
This is comprehensive packet observation of the **reached failed-open lifecycle**,
not of pipeline construction/start/reception/loss/stop, which were unreachable.

## Sanitizer boundary

The new actual stream construction/destruction path was run unsuppressed with
`TSAN_OPTIONS=halt_on_error=0:exitcode=66:second_deadlock_stack=1` to preserve the
complete report through process destruction. One lock-order-inversion warning at
exit: two main-thread mutexes, log4cpp addAppender (offset 0x1a75f), acquisition
0x1dba1, reverse removeAllAppenders 0x1a47a through 0x1d609 with prior 0x1d765.
The entire native.log is retained, not just the summary. Actual loaded identities
match the four hashes above. The external frames/order match the independently
approved cycle; the application entry frame is now native stream construction,
not the existing GenApi contract. **No new suppression/exception is introduced.**
This explicit native TSan audit remains nonpassing (exit 66), independently of the
existing narrowly scoped SDK-contract TSan gate. No unrelated finding is waived.

## Safety/tooling and regression plan

SDK-free observer unit tests execute actual packet parsing/pcap recording using
independent literal bytes, preserve unknown traffic and capture exceptions, reject
host interfaces before SDK/process access, verify exclusive loopback fixture bind,
synthetic discovery ACK and refusal of privilege writes. They are ordinary CTest
tests, not native qualification. The native audit is never auto-registered or
installed. Audit ON without explicit Pleora build fails CMake. Direct observer
execution in the host namespace is refused before native launch. Existing output
directories are not overwritten. Native failure is nonzero AUDIT_INCOMPLETE.

Run final-HEAD full CTest, serially, in nine lanes: SDK-free GCC Debug, ASan/UBSan,
TSan, Clang Debug, Aravis-only, Pleora-only, combined Debug/ASan/TSan. The default
OFF audit option stays OFF in these regression lanes; its native runs are separate.
Use build --parallel 2, `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, `TSAN_OPTIONS=halt_on_error=1`.
Full suites include controlled transport, single-owner lifecycle/uncertainty,
register/memory, dynamic profile/Cognex, navigation forwarding, and existing
fail-closed scoped GenApi TSan audit. Also run installed GCC/Clang/combined
consumers, Aravis/combined package isolation, upstream/JSON integrity, publication
history guard, documentation links and diff checks. Record exact counts, failures
and HEAD identity in the delivery PR; configured CI is not executed validation.

## Remaining gates

* A blocked: successful native Open/Close not established.
* B incomplete: no operating receiver/control-owner coexistence or loss/shutdown
  order evidence. Failed-open absence claims cannot qualify those stages.
* C failed for attempted unicast Open: extra native GVCP command observed. Other
  modes unqualified; zero socket creation not claimed.
* No native buffer delivery or resend setting/readback; all payload/loss/recycle/
  retained-stop/cancellation behavior pending. No multicast execution.
* No license failure observed, but existing SDK entitlement and production receiver
  licensing remain unresolved. Hardware, vendor isolation hook and complete
  responder/runtime support are future qualification requirements.

Architecture recommendation **C**, not integration. Passing regression suites does
not repair any native traffic or receiver qualification gate.