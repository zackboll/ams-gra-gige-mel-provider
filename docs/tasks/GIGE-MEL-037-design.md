# Register transport design (before production changes)

Base: fetched main `cf5c27f725188658f19ade4e0b4fdc3c2adf49fa`, tree
`227ab4649f5708120c45db7b732c77e7d471f2ab`. GitHub confirms PR #30 merged.

Introduce an uninstalled `device_registers.hpp`, independent of SDKs and MEL.
An optional device interface lends a checked service to a synchronous callback
under the existing device operation mutex. This serializes an entire future
multi-call transaction with configuration, acquisition transitions and close,
without changing ICameraDevice, INavigationSink or profile/module v1 contracts.
No profile retains the service or native pointer. No worker or queue is added.

Registers are host numeric uint32 values; memory is exact bytes without swapping.
Use a conservative shared GigE Vision address window of 0..UINT32_MAX: Aravis
0.8.36 accepts uint64 at its public boundary but its GVCP packet constructors
use uint32 addresses. Pleora 6.5.3.7155 uses int64 addresses/lengths, inherited
PvDevice memory operations, and uint32 register values. Check ranges before
casts. Both addresses and memory lengths must be multiples of four; length is
1..4096 bytes. Pleora PvDevice.h explicitly documents these requirements;
Aravis GVCP rounds lengths to words, so rejecting tails prevents unintended I/O.
Check overflow independently before checking the address window. Caller spans
must designate valid live storage; C++ cannot validate arbitrary dangling pointers.
Read through temporary storage and publish only after complete native success.

Keep backend Error intact inside a transport-local failure carrying a typed
write completion classification: not attempted versus uncertain after native
entry. Native failure is conservatively uncertain, including partial memory
writes; never retry or roll back at the service layer. Successful transport is
not sensor acceptance. Preserve SDK code/domain/text and existing classification.

Native qualification uses actual Aravis Fake APIs and existing acquisition/
loopback regressions. Pleora gets a narrow production-call template boundary,
compiled against actual installed declarations and exercised with recording
calls plus real disconnected PvDeviceGEV failures. No physical discovery or I/O.

Important pinned-SDK limits found during inspection: Aravis privately retries
GVCP requests, splits memory transfers, and can extend waits on pending ACKs;
its public device API offers no per-call retry/deadline override. Pleora documents
WriteMemory fallback into multiple register commands. One service call is NOT
one packet or atomic delivery. Qualification must distinguish service-level
single calls from SDK retransmission and must not assert a hard deadline that
these APIs cannot enforce. If strict no-wire-retry/hard cancellation is required,
this pin is an architecture blocker, not permission to invent SDK controls or
silently change acquisition policy.

Validation: SDK-free recording/negative/lifetime/concurrency tests; native tests;
unchanged profile/module/Cognex/navigation/acquisition regressions; full compiler/
sanitizer matrix, existing scoped GenApi audit, installed consumers, integrity,
publication and documentation checks. Serialize loopback lanes across trees.
Executed evidence and unresolved limits are recorded separately at delivery.