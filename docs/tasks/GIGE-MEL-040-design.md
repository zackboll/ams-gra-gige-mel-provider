# GIGE-MEL-040 pre-implementation design

Recorded before audit implementation. Base: fetched main
`26384976fc43cbc400e4e13c12f68f926c247e36`, tree
`30bc4842a50f832dc3085f9fe3424329cb72a147`. GitHub confirms PR #33 merged
at that commit. Branch: `feature/gige-mel-040-pleora-stream-traffic-audit`.

## Scope and evidence

Review includes README, implementation profile, single-owner decision, controlled
transport, register transport, Pleora integration, native adapter/stream/contracts,
and the installed eBUS headers, receivingdata.html, streamparametersgev.html and
MulticastSlave source. The SDK is the existing external 6.5.3.7155 installation;
matching GenApi headers and runtime remain unchanged. No proprietary source is
copied into this repository.

Implement a separate, uninstalled executable behind default-OFF
`GIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT`. It links native stream/buffer/GenICam/base
libraries, not the production adapter or MEL factories. Construct PvStreamGEV,
attempt explicit unicast Open, inspect local receiver parameters, optionally use
PvPipeline, then explicit Stop/Close and destruction. Never create PvDeviceGEV,
discover devices, configure acquisition/destination, or change licensing.

The Open string overload takes transmitter IP, local port, channel, local IP and
buffer capacity. The headers do not promise absence of internal GVCP. GetParameters
exposes receiver parameters; RequestMissingPackets is documented as Boolean.
Pipeline manages host buffers and requires no device object. Destination and
acquisition belong to a separate controller, not the stream API. MulticastSlave
uses a multicast Open overload and disables RequestMissingPackets, but its device
selector is discovery: do not execute that shipped program.

## Isolation and observation

A fresh Linux user/network namespace is supported on this host. Bring up only lo;
verify interface inventory and routes before launching any native binary. Use a
test-owned numeric 127/8 endpoint, never a physical device. Check exclusive bind
of UDP 3956 before serving; no SO_REUSEADDR. Keep serial resource policy with the
existing Aravis fixed-port fixture; do not change controlled transport's 3956 ban.

Independent full-interface packet capture must start before process creation and
end after native process exit, covering loader initialization and static destruction.
No GVCP-only capture filter: preserve unknown IP/UDP/non-UDP packets as evidence.
Inspect /proc process-owned socket links and UDP tables at lifecycle barriers;
short-lived sockets remain an observation limit unless independently traced.
Capture readiness, drops, errors and process return codes are qualification inputs,
not a passing test merely because an executable exits zero. Keep raw captures,
runtime inventories and logs outside the repository.

First compare no responder and a minimal read-only GVCP responder, default and
explicitly disabled resends. Respond only to understood commands, retaining exact
wire requests. Do not substitute the 039 fixture identity register. If Open cannot
establish a native stream over loopback, preserve exact failure and stop short of
claiming receive/coexistence qualification. A future synthetic GVSP sender must
deliver real buffers before reception, loss recovery or retained data can pass.

## Independent gates and fail-closed policy

* A: actual native construction, successful stream-only Open and Close without an
  application control connection.
* B: comprehensive evidence of no unauthorized ownership/configuration/acquisition
  writes or competing heartbeat throughout the qualified lifecycle.
* C: no additional command authority/socket or GVCP command traffic. Read-only
  reachability/capability queries fail C even when B can pass.

Unavailable comprehensive observation means AUDIT_INCOMPLETE. Unestablished native
delivery means NATIVE_RECEIVE_PENDING. Licensing refusal is a runtime block, not
ownership incompatibility. No watermark changes, SDK binary changes or entitlement
inference. TSan runs unsuppressed first; the existing approved external-cycle
exception remains restricted to its current contract. Any new exception needs its
own identity and exact-cycle justification; otherwise native sanitizer gate blocks.

## Validation and delivery

Exercise audit safety/refusal paths as well as real native execution. Run all nine
requested regression lanes serially for network fixtures, plus installed consumers,
package isolation, upstream/JSON/publication and documentation/diff checks on final
HEAD. Report configured versus executed separately. Record exact final commit/tree
on the delivery PR, avoiding self-referential hashes in committed documentation.
Production adapters, provider surfaces and Aravis passive-path conclusions do not
change. Recommend production work only after packet-backed gates; otherwise outcome
C with explicit missing qualification. Push one branch and open one non-draft PR;
never merge, approve or enable auto-merge.