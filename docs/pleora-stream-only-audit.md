# Pleora stream-only traffic investigation — GIGE-MEL-040

**Disposition: AUDIT_INCOMPLETE / NATIVE_RECEIVE_PENDING. Architecture outcome C.**
The actual native unicast Open path transmits a GVCP discovery command even without
an application-side PvDeviceGEV. It fails to establish a loopback data receiver.
This is a useful negative traffic finding, **not qualified passive reception**.
No production adapter, MEL factory, profile, navigation or timestamp changes.

## API evidence and actual use

Inspected external eBUS **6.5.3.7155** headers `PvStreamGEV.h`, `PvStream.h`,
`PvPipeline.h`, `PvGenBoolean.h`, installed `share/doc/sdk/receivingdata.html`,
`streamparametersgev.html`, `class_pv_stream_g_e_v.html` and
`share/samples/MulticastSlave/MulticastSlave.cpp`. Proprietary material remains
external. Existing provenance/licensing limitations in [Pleora integration](pleora-integration.md)
still apply; mirror consistency is not proof of vendor entitlement.

| Public operation | Installed declaration/behavior evidence | Executed here |
| --- | --- | --- |
| PvStreamGEV constructor/destructor | Native receiver object, independent of PvDevice | Yes, including destruction |
| Unicast Open | Returns PvResult; string IP overload with uint16 local port, uint16 channel=0, const PvString local IP, uint32 capacity=64; same-subnet receiver | `Open(PvString("127.0.0.2"), uint16_t(41040), 0, PvString("127.0.0.1"), 16)` |
| Other Open forms | String-only, device-info forms, multicast group/data-port form | Inspected, not executed |
| Close | Inherited PvStream method, returns PvResult; closes receiver | Executed after failed Open; NOT_CONNECTED |
| GetLocalIPAddress / GetLocalPort | Return PvString / uint16_t receiver properties; port zero in Open requests automatic allocation | Compiled; runtime branch not reached |
| GetParameters | Returns PvGenParameterArray*, local receiver settings/statistics, not camera node map | Compiled; runtime branch not reached |
| RequestMissingPackets | PvGenBoolean in shipped sample; receiver docs say enabled requests missing packets; retry/group/timeout/pending/delay settings also apply | Native availability/default/write/readback **pending** |
| PvPipeline(PvStream*) | Owns/manages buffer pool, not stream ownership; no PvDevice argument | Compiled, not constructed because Open failed |
| Pipeline lifecycle | SetBufferCount returns PvResult, SetBufferSize is void; Start creates buffer-handling thread; RetrieveNextBuffer has ms timeout and separate operation result; ReleaseBuffer recycles; Stop returns PvResult | Compiled optional path, not executed |

The sample opens multicast `239.192.1.1:1042`, starts a pipeline, and disables
RequestMissingPackets. It performs device selection/discovery first. **The shipped
sample was not executed.** Headers mark the device-info local-IP argument deprecated
and ignored; string overload documentation still describes local-IP selection.
Do not depend on that argument to enforce isolation: the fresh namespace enforces it.

Receiver construction/Open/Close and host pipeline calls take no connected device
object. The normal device-side operations Connect/access selection, GenICam camera
configuration, SetStreamDestination, StreamEnable and AcquisitionStart/Stop belong
to PvDevice/PvDeviceGEV and a separate authority. This executable calls none of them,
does not link the production backend, and never constructs PvDeviceGEV. That does
**not** imply the native stream API is GVCP-silent.

## Isolation, independent capture and scope of observation

The explicit [observer](../scripts/pleora_stream_audit.py) creates a fresh Linux
user/network namespace using `unshare --user --map-root-user --net`. It inventories
the current namespace with if_nameindex/ip, brings up only lo, and records all
routes. No physical NIC, default route, discovery broadcast or multicast transmitter
is introduced. No host networking, camera address or SDK license file changes.
The read-only fixture binds **exclusively** to `127.0.0.2:3956`, after a free-port
probe, with no reuse option. Separate namespaces prevent interference with the
existing Aravis fixed-port simulator; runs are also serial. Existing socket policy
and the 038/039 transport's rejection of port 3956 remain unchanged.

An independent Python process uses Linux **AF_PACKET/SOCK_RAW/ETH_P_ALL**, unbound
and **without any capture filter**, to observe every namespace interface. It opens
the capture socket before spawning the native executable and closes it after
process exit/static destruction plus a 300 ms observation tail. It retains full
frames in pcap and JSONL, including unknown/non-GVCP traffic. Linux loopback supplies
outgoing and host-delivery copies: count packet_type=4 once for transmission, not
both copies as retransmissions. PACKET_STATISTICS records kernel counts/drops;
capture/responder errors or drops fail observation. An offline tcpdump decode
independently checks retained pcap interpretation. Application stage messages are
only timing barriers, **not evidence of SDK silence**.

At each reachable stage the observer inventories native process `/proc/PID/fd`,
fdinfo and namespace UDP/UDP6/TCP/TCP6/raw/packet/netlink/unix tables. Native memory
maps and hashes establish loaded SDK/runtime identities. Barrier snapshots cannot
identify short-lived command descriptors that are created and closed inside Open;
there is no syscall trace here. Therefore **no claim of zero extra socket creation**
is made. The captured command's distinct UDP source endpoint itself disproves zero
GVCP command traffic in the attempted unicast lifecycle.

## Native results and all observed outgoing GVCP types

Both absent responder and synthetic successful DISCOVERY_ACK responder cases give:

```text
Open: code=25 NOT_FOUND
Unable to open the stream. Unable to find a suitable data receiver.
Close: code=23 NOT_CONNECTED
Unable to close stream. Not connected.
```

The SDK independently emits UDP syslog diagnostics saying “Could not reach device,
data receiver factory create failed.” A 248-byte synthetic discovery response is
visible returning to the SDK endpoint; that does not prove SDK acceptance of its
identity/layout or distinguish loopback enumeration from reachability rejection.
No vendor identity was impersonated to obtain a license. Do not conclude that a
complete real-device responder or all loopback arrangements are unsupported.

| SDK-generated GVCP command type | Trigger | Destination | Flags / request ID / payload | Observed count |
| --- | --- | --- | --- | --- |
| DISCOVERY_CMD `0x0002` (unicast reachability query, not broadcast) | Inside native Open, before it returns failure | `127.0.0.2:3956` from `127.0.0.1:ephemeral` | `0x4201` / `0xb001` / empty; exact bytes `420100020000b001` | One per executed attempt |

The fixture, **not SDK**, sends DISCOVERY_ACK `0x0003`, status zero, matching ID,
248-byte bootstrap payload. An earlier minimal negative-ACK probe also failed;
it is not evidence that a functioning responder was tested. No additional SDK GVCP
types occur in these bounded failed-open captures: no register read `0x0080`,
register write `0x0082`, memory read `0x0084`, memory write `0x0086` or packet resend
`0x0040`. This absence applies only to the observed failed-open interval, all
interfaces of its private namespace, the native process and fixture endpoints;
**not** an operating receiver, pipeline, packet-loss scenario or owned controller.
No privilege/heartbeat/destination/acquisition operation was observed in that
limited interval. B remains incomplete, not passed by lack of those operations.

Other captured traffic is SDK UDP syslog to `127.0.0.1:514` and kernel ICMP port
unreachable replies (including absent GVCP responder). No unclassified frames in
the completed recorded attempts. The loader keeps one socket FD at barriers; its
inode is not found in the namespace protocol tables, so its family/purpose remains
unclassified. No persistent UDP command socket survives at those barriers. This
does not hide the transient outgoing GVCP endpoint.

## Resends, reception and coexistence boundaries

The `default` and `disabled` requested modes both fail **before** GetParameters or
RequestMissingPackets access. The latter is **not an executed disabled-resend
configuration**. Disabling resends cannot be claimed to remove the observed Open
query. Native parameter availability, actual default/readback, loss-triggered resend
behavior and any remaining GVCP while receiving remain unqualified.

No synthetic GVSP sender or controller-coexistence implementation was added after
Open failed. Zero native received buffers, frame IDs, image representations or
retained-after-stop pixels are claimed. Receive timeout, incomplete frames, buffer
recycling, cancellation/bounded stop, controller privilege/heartbeat/release and
both shutdown orders are **NATIVE_RECEIVE_PENDING / NOT EXECUTED**. The existing
039 fixture-only ControlOwner was not connected to this endpoint or a camera.
Multicast API was inspected separately but not executed or qualified. There was no
license-result block in the reached path; no NO_LICENSE result establishes neither
entitlement nor how a working receiver would be licensed. Watermarks are unchanged.

## Independent dispositions and recommendation

| Gate | Disposition | Reason |
| --- | --- | --- |
| A — stream-only API | BLOCKED at successful Open/Close qualification | Actual native construction/Open attempt/Close/destruction executed without PvDeviceGEV; Open NOT_FOUND, Close NOT_CONNECTED |
| B — no unauthorized camera control | AUDIT_INCOMPLETE | Failed-open trace has no control writes or heartbeat, but running/pipeline/loss/coexistence lifecycle was not reached |
| C — strict single-GVCP-socket | FAIL for tested unicast Open path | Actual SDK-generated GVCP discovery command from separate UDP endpoint; no zero-socket-creation claim; other modes remain unqualified |

**Outcome C: passive behavior cannot be qualified using the current tested
SDK/runtime/loopback fixture.** Outcome B would require a usable native receiver,
which this experiment did not establish. Preserve the strict Gate C criterion;
do not classify the discovery query as harmless authorization. A future reviewed
investigation needs vendor-supported isolated reachability/receiver operation,
complete native receive/loss/ownership evidence, and license entitlement. Any
relaxation permitting read-only queries needs separate architecture review. No
production integration recommendation is justified. This finding does not change
the earlier conclusion that inspected Aravis public APIs lack a supported passive
receiver seam; no Aravis fork or artificial two-SDK symmetry is introduced.

See [design](tasks/GIGE-MEL-040-design.md) and [execution/validation](tasks/GIGE-MEL-040-validation.md).

## Explicit reproduction (never run on a physical camera)

All paths below are examples for this recorded workstation; choose new private
output directories. Ordinary CTest never launches this native audit.

```sh
cmake -S /home/zboll/git/ams-gra-gige-mel-provider \
  -B /home/zboll/git/ams-gra-gige-mel-provider/build/040-audit -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DGIGE_MEL_BUILD_PLEORA=ON \
  -DGIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT=ON \
  -DGIGE_MEL_PLEORA_SDK_ROOT=/home/zboll/sdk/pleora/ebus_sdk \
  -DGIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR=/home/zboll/sdk/gige-mel-027-genapi/headers/library/CPP/include
cmake --build /home/zboll/git/ams-gra-gige-mel-provider/build/040-audit \
  --target pleora_stream_only_audit --parallel 2
python3 /home/zboll/git/ams-gra-gige-mel-provider/scripts/pleora_stream_audit.py \
  --sdk /home/zboll/sdk/pleora/ebus_sdk \
  --binary /home/zboll/git/ams-gra-gige-mel-provider/build/040-audit/pleora_stream_only_audit \
  --output /tmp/new-private-040-evidence --mode default --responder
```

Omit `--responder` for absent-device reachability, use `--mode disabled` only as a
requested post-Open setting. Missing namespace/capture/SDK prerequisites fail;
nonzero native result is AUDIT_INCOMPLETE, not a silently passing skip. Artifacts
contain host paths/SDK diagnostics and are **not approved for publication**. Never
commit captures, SDK files or generated synthetic images.