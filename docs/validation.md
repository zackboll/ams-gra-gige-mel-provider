# Bootstrap validation record

## GIGE-MEL-012 confirmed Aravis control loss

**Starting revision.** Fetched `origin/main` was exactly commit
`b2c1002de784af0e12c82a9a6aff12ca56c03426`, tree
`8d425a2d2e2ef077309cb946c205a114ce01730f` (merged PR #11). The working tree
was clean. Work starts on `feature/gige-mel-012-control-loss`; no completed
GIGE-MEL-008–011 correction is intentionally removed. The unchanged starting
adapter is built separately in an external detached worktree.

**Scope.** The only production behavior change is private Aravis signal/state
and native-lifetime handling in `src/backends/aravis/aravis_backend.cpp`.
`aravis_private.hpp` clarifies the existing private native-reference restriction.
The existing network contract/fixture gains Q1–Q6 and explicit server-side
privilege revocation/reset. Public `backend.hpp`/`provider.hpp`, `provider.cpp`,
timestamp descriptors, SDK pin, vendored bytes, build options, CTest target count,
the five hosted jobs and both suppression files are unchanged. Current behavior
is documented in the integration guide, backend guide, architecture,
implementation profile, README and surface roadmap. This does not add a MEL
surface, provider C ABI, reconnect mechanism or full-conformance claim.

### Native signal and before/after control

Inspected the requested pinned Aravis 0.8.36 sources at
`97ff5e330919d7a44b871df4eda669feeafdf342`, including signal registration,
heartbeat, GVCP failure/readback behavior, public Fake privilege API, simulator
controller bookkeeping and native stream/device finalization. Also inspected
GLib 2.84.4 `gobject/gsignal.c` and `gclosure.c` for closure/invocation lifetime
and temporary signal-instance references. Details and the limits of silent
network-failure detection are in
[the integration contract](aravis-integration.md#confirmed-camera-control-loss-gige-mel-012).

The local SDK was reprovisioned with the existing checksum-verifying script,
unmodified source and no SDK instrumentation, into `/tmp/gm12/aravis-prefix`.
Its archive SHA-256 remains `246deaa0042a387ff1bc00332d0fb80537ce14abde2c28d1a54c91f17adc51bf`.
Every SDK test prints runtime/compiled **0.8.36** and the loaded
`/tmp/gm12/aravis-prefix/lib/libaravis-0.8.so.0`. Local compilers are GCC 14.2.0
and Clang 19.1.7. Missing development prerequisites were extracted outside the
repository; no package or SDK artifacts enter commits.

The same Q1 control source, unchanged private headers, real simulator fixture
and virtual-clock support were separately compiled against the **starting-main
static adapter and provider**, using that same SDK. It reported:

```text
SDK control-lost emitted after server CCP readback zero; heartbeat detection 802 ms
FAIL Q1 ... timed out waiting for terminal receive after signal
```

The failure is the intended missing signal-to-terminal translation, not SDK
loading, private header incompatibility or a simulator setup failure. The
failure cleanup cancels before joining, so the baseline does not wait 60 s.
Corrected production passes the same primary control with `make_backend()`.
Representative corrected measurements: 785–804 ms from server revocation to
SDK detection and 0–4 ms from actual SDK event to receive completion. Assertions
separate the heartbeat detection bound (10 s) from event-handling latency
(500 ms; public receive deadline 60 s). These are simulator measurements, not
physical-camera latency guarantees.

### Regression evidence

- **Q1:** genuine GVSP Mono8 frames, all four source leases held, counted empty
  native wait, actual SDK signal after public server-side CCP revocation and
  zero readback, terminal `disconnected`, exact control-loss text, zero timeout
  increment, no restart/configuration/second-wrapper bypass, later successful
  native read does not heal state, held bytes survive all owners, cross-thread
  release and explicit new-open acquisition.
- **Q2:** observation-only hooks prove no configuration writes or stream
  creation after loss; no adapter AcquisitionStop after loss; native stream
  precedes device finalization; one remaining source buffer survives all
  external owners, is immutable and is freed exactly once on another thread,
  never requeued.
- **Q3:** actual heartbeat emission overlaps close, three times per scenario.
  A bounded **test-only** subscriber holds emission in flight; native stream
  finalization is observed before releasing it and joining close. Teardown
  completes, no native resources remain, close is idempotent, and a fresh
  production logical open/acquisition cannot inherit the old signal.
- **Q4:** unchanged provider, existing test virtual clock, genuine network
  bytes and correct headers before loss; `stream_failures == 1` plus diagnostic,
  no new Image publication after fault (including a released destination),
  registered bytes unchanged, faulted enable/config rejected, explicit disable
  drains and source leases return. No `provider.cpp` defect was reproduced.
- **Q5:** actual popped frame paused before admission, actual heartbeat signal,
  zero delivered leases and timeouts; no edited native facts or injected flag.
- **Q6:** real signal during native-open validation refuses open and safely
  retires the native device/reservation; a subsequent independent open succeeds.
- Existing **L** proves complete GVSP loss still yields three bounded ordinary
  timeouts with valid CCP, no control-lost signal, successful configuration
  readback and deliberate cancellation classified `stopped`.
- Existing **D2**, SDK command-failure, timestamp and embedding contracts retain
  their cancellation, uncertain-command recovery and invalid-time behavior.
  Production Aravis frames still carry exactly `FrameTimestamp{}`. Positive MEL
  publication is only under the test virtual clock; the existing negative
  production-time control still publishes nothing and leaves bytes untouched.

**Failure investigated and corrected.** The first controlled close/emission
overlap reproduced `g_system_thread_wait: Resource deadlock avoided` on the
heartbeat thread. GLib's emission reference became the final device reference
and invoked the SDK finalizer, which joins that same heartbeat. Close now
disconnects, observes closure detachment and drains the emission instance
reference before dropping its native reference and reservation. A Fake-only
failed-command readback test also exposed why this GigE reference handshake
must not be applied to retained Fake test handles; that test policy remains
unchanged. Intermediate tests of the old binary are not final validation.

**Network test scheduling.** Two early local full-suite runs collided because
an older suite was still using port 3956. CTest's resource lock is per build
directory, not cross-process/cross-build isolation. Final local SDK-enabled
suites and repeats run serially across build directories. Port conflicts fail
explicitly, are not skipped, and are not treated as feature failures or passes.

### Validation execution

SDK-free GCC development CTest passed **8/8**. SDK-enabled GCC development,
Release, Clang development, ASan/UBSan and TSan are exercised with **14** CTest
entries, including the extended existing loopback target. The final-source
matrix and bounded repeat results are recorded below after completion.
ASan uses `detect_leaks=1:halt_on_error=1`; UBSan uses
`halt_on_error=1:print_stacktrace=1`; TSan uses `halt_on_error=1` and the
**unchanged** documented SDK suppressions. The SDK itself is not instrumented.
No new suppression or weakened sanitizer option was introduced.

**New TSan report investigated.** Q6 initially reported project callback user
data initialization versus its first heartbeat callback (`shared_ptr::get`,
allocation in `NativeOpen::connect_loss`). GLib's registration lock is in the
uninstrumented SDK stack. Rather than suppressing the report, the production
latch and immutable callback owner are now allocated **before** native open
creates the heartbeat thread, giving their initialization a thread-creation
publication edge as well as GLib's signal-registration synchronization. Q6's
test subscriber follows the same pre-thread initialization order. No suppression
pattern or callback lock was added; final-source TSan is rerun below.

A bounded TSan repeat then exposed the analogous initialization report in the
**second, test-only** SDK subscriber (`LossSignal::notified`, allocation in
`observe_loss`), not the production callback. Production-factory tests attach
this observer after opening the device, so its immutable owner publication is
now explicitly synchronized by a test-only C++ mutex, with a pinned owner copied
before recording the event. Its destroy notifier uses the same publication
mutex. The production signal callback remains a single atomic store and never
takes that mutex. This project-involved report was fixed without any new SDK
suppression; the final observer version is rerun under TSan.

| Final production/test source configuration | Result |
| --- | --- |
| SDK-free GCC development | **8/8** |
| SDK-enabled GCC development | **14/14**, 92.40 s |
| SDK-enabled GCC Release | **14/14**, 92.53 s |
| SDK-enabled Clang development | **14/14**, 92.52 s |
| SDK-enabled ASan/UBSan | **14/14**, 93.05 s |
| SDK-enabled TSan, final test observer, unchanged suppressions | **14/14**, 93.21 s; no report |

The final test-observer change is separately revalidated across the matrix;
it does not alter production behavior. TSan Q1–Q6 ×3 passed with no report.
ASan/UBSan and GCC development Q1–Q6 ×3 also passed
(18 scenario executions, including nine controlled close/emission overlaps).
Each scenario has a 90 s watchdog and each repeated invocation an outer 180 s
bound. These are focused repetitions, not a mutation campaign.

Both offline dependency verifiers passed; all **29** package-isolation verifier
tests passed. Actual ordinary SDK-enabled installs (GCC Debug, Release and
Clang) passed the fail-closed package verifier: correct SDK resolution, no
provider Aravis/GLib dependencies, 157 installed paths, 97 checked text files,
two installed binaries without Aravis symbols, and a separately configured,
built and executed installed consumer with `PKG_CONFIG_PATH` removed.
Diagnostic `--help` and `git diff --check` passed.

The wildcard **255.255.255.255:3956** simulator discovery listener remains an
explicit test-only network exception, not loopback confinement or an isolated
namespace. Existing inode/socket audits and unrelated-listener negative
controls remain. TSan's unchanged loopback suppression file hides intercepted
SDK-internal accesses, including a blind spot across the SDK buffer handoff;
project/project reports remain fatal. Functional byte-stability assertions do
not substitute for instrumenting the SDK. No new project-involved sanitizer
report has been dismissed as an SDK issue.

**Limitations:** no physical camera/NIC was used. Privilege revocation is not
physical unplug/power/carrier detection, and a signal is not promised for every
network outage. The test explicitly resets the simulator's acquisition register
server-side after failed-client close before recovery; it does not infer camera
stop or add automatic reconnect. Device/stream finalizers themselves may issue
SDK I/O and retries. Timestamp qualification and full MEL conformance remain
open. Final-head push/pull_request hosted results are handoff evidence, separate
from local execution; READY_FOR_REVIEW requires all five jobs in **both** runs.

## GIGE-MEL-011-R1 simulator wildcard discovery listener (2026-11-02)

**Disposition.** This is one ordinary corrective commit on
`feature/gige-mel-011-gvsp-loopback` (PR #11). It resolves the single
remaining independent-review finding: the pinned network simulator binds a
global discovery socket to 255.255.255.255:3956. That socket is effectively
wildcard on Linux, but the earlier socket check permitted it and the earlier
record described the test environment as loopback-only. **The exposure is
documented and audited, not removed.** Network isolation is not established.
The test-only exception awaits the repository owner's acceptance.

**Starting point (verified before editing).**

| Item | Value |
| --- | --- |
| Reviewed head = local = `origin/feature/gige-mel-011-gvsp-loopback` = PR #11 head | `abc63deb71e1bce9147e903d1259634200d842e2` |
| Reviewed tree | `ddbe66492d67aca8431ba90fa18ba30695ed5a32` |
| `origin/main` (not advanced) | `e3e1b44b2ff7234c0758e090f645230df79c0645` |
| PR #11 | open, non-draft, unmerged, auto-merge off; push and pull_request runs on `abc63deb` green |
| Working tree | clean (isolated worktree) |

The branch had not advanced, so there was nothing to reconcile.

**Scope.** These files changed:

- `tests/support/aravis_gv_loopback.{hpp,cpp}` and
  `tests/aravis_gige_loopback_contract.cpp` (test-only, uninstalled);
- `README.md`, `docs/aravis-integration.md`, `docs/architecture.md`,
  `docs/mel-surface-roadmap.md` and this record, in the network-isolation
  passages only.

These are unchanged: `src/`, `include/`, the production backend and
`src/provider.cpp`, leases and the acquisition/recovery state machine,
production timestamps, the test-only virtual clock, the SDK pin and
provisioning, the factories, both TSan suppression files, sanitizer settings,
the workflow, CMake/CTest wiring, the installed package and the Python
verifier and its tests.

### Superseded statements

These earlier GIGE-MEL-011 statements are **superseded**. The historical text
below is kept unchanged:

- "Throughout, every UDP socket the process owned was on 127.0.0.1 or
  255.255.255.255" (GIGE-MEL-011 "Real GVSP"). This was a true observation,
  but it was presented as loopback evidence. The 255.255.255.255 socket is
  wildcard-bound.
- "Discovery broadcasts therefore never leave loopback" (integration guide,
  fixture header). This is true of the **client's** discovery only.
- The socket check `require_loopback_only_sockets()` (PR description: "every
  socket the process owns is on loopback"). It accepted **any** socket on
  255.255.255.255, regardless of owner, port or options. It has been replaced.
- "TSan: … the loopback-only suppression file" (heading below). The file is
  specific to the loopback **executable**. The phrase does not describe
  network confinement. The TSan decision and its limitation are unchanged.

### Pinned SDK behavior (verified)

The pinned `src/arvgvfakecamera.c` was downloaded from GitHub at commit
`97ff5e330919d7a44b871df4eda669feeafdf342`. It is byte-identical (SHA-256
`2a7366a77e15…40ef8de37`) to the file in the checksum-verified tarball
(`246deaa0…51bf`). In `arv_gv_fake_camera_start()`, lines 516-537 create:

| Socket | Pinned lines | Bound to | Observed options |
| --- | --- | --- | --- |
| A. GVCP | 518-520 | 127.0.0.1:3956 | no `SO_REUSEADDR`/`SO_REUSEPORT`, no device |
| B. GVSP output | 516-517 | 127.0.0.1:kernel-assigned (e.g. 43201) | no reuse, no device |
| C. Global discovery | 522-526 | **255.255.255.255:3956** | `SO_REUSEADDR=1`, `SO_REUSEPORT=1` (GIO `g_socket_bind(…, allow_reuse=TRUE)`), **no `SO_BINDTODEVICE`** |
| D. Subnet discovery | 530-536 | — | **not created**: `lo`'s `ifu_broadaddr` is 127.0.0.1, which equals the GVCP address (`src/arvnetwork.c:380-381`) |

`arv_gv_fake_camera_stop()` (lines 571-591) joins the simulator thread, then
closes A, C and B.

**Linux semantics.** ip(7) on this host says: "INADDR_BROADCAST
(255.255.255.255) has the same effect on bind(2) as INADDR_ANY for historical
reasons." Kernel v6.12 `inet_bind()` stores the bound address as the receive
address, and `__udp_is_mcast_sock()` checks a device only if
`SO_BINDTODEVICE` is set. The following were executed locally with
out-of-repository probes:

- `/proc/net/udp` shows `FFFFFFFF:…` and `getsockname()` returns
  255.255.255.255, **not** 0.0.0.0;
- without reuse, 255.255.255.255:P conflicts with 0.0.0.0:P and with
  255.255.255.255:P, but not with 127.0.0.1:P;
- the socket received a limited broadcast sent from 127.0.0.1 via `lo`;
- the pinned simulator answered a GVCP READREG sent from 127.0.0.1 to
  255.255.255.255:3956. The reply came from 127.0.0.1:3956 with value
  127.0.0.1, so C is a live control endpoint;
- it did **not** answer a unicast to the host's own non-loopback address.

No datagram was sent to or from an external interface or another host. The
host firewall could not be read without root, and none is relied on.

**Distinctions.**

- *Intended outgoing traffic is loopback.* The client's discovery sockets
  are bound to 127.0.0.1, and `ip route get 255.255.255.255 from 127.0.0.1`
  gives `dev lo`. GVCP and GVSP use 127.0.0.1 only.
- *The binding scope is wildcard.* C is bound on all interfaces of the
  network namespace for each simulator's lifetime.
- *Unsolicited non-loopback reception is possible.* A GVCP datagram addressed
  to 255.255.255.255:3956 that reaches this host on any interface can be
  delivered to C. The simulator would answer it.

### Audit and control results

The audit contract is described in
[Socket audit](aravis-integration.md#socket-audit-contract-gige-mel-011-r1).
The following are from the executed GCC dev run; inodes and ephemeral ports
vary per run.

- **After simulator start (A):** 3 UDP sockets owned:
  - loopback-bound 2: `127.0.0.1:3956` and `127.0.0.1:43201`, both
    `SO_REUSEADDR=0 SO_REUSEPORT=0`;
  - documented global-discovery listener 1 (expected 1):
    `255.255.255.255:3956 (inode 106881310, fd 5, UDP datagram,
    SO_REUSEADDR=1, SO_REUSEPORT=1, SO_BINDTODEVICE=none)`;
  - unexpected 0.

  The exception was observed and attributed to `src/arvgvfakecamera.c:522-526`
  and ip(7). `lo` broadcast is 127.0.0.1, so there is no subnet-discovery
  socket. The listener is never counted as loopback.
- **During GVCP/GVSP acquisition (C):** 5 sockets owned. Loopback-bound 4:
  simulator GVCP and GVSP output, the client's GVCP socket and the client's
  GVSP socket (the SCP0 port). Listener 1, unexpected 0.
- **After client close (A):** only the simulator's 2 loopback sockets plus
  the listener remain.
- **After simulator destruction (every `finish()`, every case):** 0 UDP
  sockets, listener 0 (expected 0), unexpected 0, and the recorded inodes are
  gone. `bind(127.0.0.1:3956)` and a no-reuse `bind(255.255.255.255:3956)` both
  succeed. The discovery interface is restored and read back.

**Negative controls (case S, executed).** With the simulator running, each of
the following was bound in this process in turn:

- `0.0.0.0:43818` (no reuse);
- `255.255.255.255:48706` (reuse);
- a lookalike `255.255.255.255:3956` with the simulator's exact options.

Each time the audit reported `unexpected 1 [<that socket>]` and still showed
`documented simulator global-discovery listener 1 [255.255.255.255:3956
(inode …, fd 5 …)]`, giving `FAIL`, and the checked form threw. After each
socket was closed the normal audit passed again. Seven forged variants of
the listener (0.0.0.0, port 3957, a different inode, no `SO_REUSEPORT`,
`SO_BINDTODEVICE=lo`, non-UDP, IPv6), as well as the real listener with no
live simulator, all classified as `unexpected`. No datagram was sent.

**Mutation controls (out-of-repository builds with the exact dev
compile/link commands; the repository and build trees were not modified).**

- *Blanket rule:* any 255.255.255.255 socket counted as the exception.
  Case S fails: `classify_udp_socket(listener, nullptr) ==
  SocketClass::unexpected`.
- *Reviewed `abc63deb` rule:* 127.0.0.1 or 255.255.255.255 counted as
  loopback. Case S fails at simulator start: "the global-discovery socket does
  not classify as the documented exception".

**External-holder control.** A separate socket held 255.255.255.255:3956
(reuse) in another process. Scenario A then failed **before** starting the
simulator: "UDP 255.255.255.255:3956 cannot be bound before the simulator
starts: Address already in use …". After the holder was released, A passed.
This shows that the start and release probes are namespace-wide.

**Functional regression and sanitizers (executed, sequentially, after a full
rebuild of every tree).**

| Build | SDK-free CTest | SDK-enabled CTest | `aravis_gige_loopback_contract` |
| --- | --- | --- | --- |
| GCC dev | **8/8** | **14/14** | 10/10 transport + S, 42.6 s |
| GCC Release | **8/8** | **14/14** | 10/10 + S |
| Clang 19 dev | **8/8** | **14/14** | 10/10 + S |
| ASan/UBSan (`detect_leaks=1:halt_on_error=1`, UBSan `halt_on_error=1`) | **8/8** | **14/14** | 10/10 + S, no report |
| TSan (`halt_on_error=1`, the unchanged committed suppression files) | **8/8** | **14/14** | 10/10 + S, no report |

No GLib/Aravis warning or critical was printed. The executable aborts on
any. The ten transport groups and their evidence are unchanged: real GVCP
configuration, real GVSP pixels and native frame IDs, lease ownership,
cancellation and restart, `capture_one()`/`time_probe()`, positive
virtual-clock MEL publication, negative production-timestamp admission, and
simulated packet loss. CTest totals are unchanged: SDK-free **8**,
SDK-enabled **14**. S runs inside the existing CTest entry.

**Bounded repetition.** The full loopback executable ran 3× (GCC dev) and
2× (TSan) after the matrix. Every run passed 10 transport scenarios + S, and
every listener was identified and released.

**Other checks (executed):**

- `vendor_upstream.py --verify` (97 files) and `verify_json.py` (2 files),
  both offline;
- all 29 package-isolation verifier tests;
- the real `verify_package_isolation.py` on fresh dev, Release and Clang
  SDK-enabled installs: all six checks `ok`, ending "package isolation
  verified", including the separately built consumer;
- the SDK-free installed consumers (dev, Release, Clang) pass, and the
  SDK-free and SDK-enabled install file sets are identical;
- `gige_mel_aravis_diagnostic --help` in all five SDK builds;
- `git diff --check`.

### Remaining limitations

- **The exposure is not removed.** While a simulator is alive, about 45 s per
  full run, a wildcard GVCP listener on 255.255.255.255:3956 exists. It
  answers GVCP discovery and register/memory commands that reach it, and
  accepts writes while no controller holds CCP.
- Nothing shows the simulator was **unreachable** through another interface
  while it ran. The cleanup checks show release only.
- No `SO_BINDTODEVICE`, network namespace, firewall rule or VM is
  implemented or validated. The host firewall was not read. **GitHub-hosted
  runners are not claimed to isolate the listener.** Their isolation was not
  verified.
- Remote delivery was not tested: no datagram was sent from another host or
  received on an external interface.
- The audit covers sockets owned by **this** process. The namespace-wide
  bind probes cover only port 3956 on 0.0.0.0/255.255.255.255.
- **Owner acceptance of this test-only network exception is pending.**
- The TSan limitation of GIGE-MEL-011 (`called_from_lib` blind spot at the
  GigE buffer-handoff boundary) is unchanged and still applies.

## GIGE-MEL-011 loopback GigE Vision transport (2026-11-02)

> **R1 note.** This historical record is kept unchanged. Its statements that
> the sockets were "on 127.0.0.1 or 255.255.255.255" and that discovery is
> loopback-only describe the client and the intended traffic. They are **not**
> network-isolation evidence, and are superseded by
> [GIGE-MEL-011-R1](#gige-mel-011-r1-simulator-wildcard-discovery-listener-2026-11-02).
> The transport, sanitizer and TSan results below remain valid.

**Starting point.** `origin/main` was freshly fetched and verified before
work began. It had not advanced from the task's expected revision. The work
was done in a clean, isolated worktree on branch
`feature/gige-mel-011-gvsp-loopback`.

| Item | Commit | Tree |
| --- | --- | --- |
| `origin/main` (includes merged PR #10) | `e3e1b44b2ff7234c0758e090f645230df79c0645` | `0077427592fcd87f822af166edfd0a50754b51a3` |

**Scope.** These files were added, all test-only and uninstalled:

- `tests/aravis_gige_loopback_contract.cpp`;
- `tests/support/aravis_gv_loopback.{hpp,cpp}`;
- `tests/support/tsan_gige_loopback_sdk.cpp`.

These were changed:

- `cmake/GigeMelAravis.cmake`, which adds one test target and the CTest entry
  `aravis_gige_loopback_contract` (TIMEOUT 600,
  `RESOURCE_LOCK gige_gvcp_loopback_3956`);
- the documentation.

These are unchanged:

- `include/gige_mel/*`, `src/provider.cpp` and every file under `src/`;
- the production `FrameTimestamp{}`, common timestamp admission, the Aravis
  acquisition/recovery state machine (GIGE-MEL-009-R1) and lease ownership;
- `tools/aravis_diagnostic.cpp`, the factories and `getAPI_Manager()`;
- `tests/support/aravis_virtual_clock.*` and `aravis_fake_support.*`, both
  reused unchanged;
- `tests/support/tsan_uninstrumented_sdk.cpp` and its two patterns, used by the
  three earlier executables;
- the workflow, presets and `TSAN_OPTIONS`;
- the SDK pin, provisioning, manifests, verifier and its 29 tests, and
  Python 3.9.

No genuine network-path defect was reproduced, so no production code changed.

**Counts.** SDK-free CTest: **8** (unchanged). SDK-enabled CTest: **14** (13
before).

### Pinned sources

`src/arvgvfakecamera.{c,h}`, `src/arvgvinterface.c`, `tests/fakegv.c`,
`src/arvgvstream.c`, `src/arvgvdevice.c`, `src/arvfakecamera.c` and
`src/arvnetwork.c` in the working extraction are byte-identical to a fresh
extraction of the checksum-verified archive (SHA-256 `246deaa0…51bf`, release
commit `97ff5e33…`). For example, `arvgvfakecamera.c` is `2a7366a7…`,
`arvgvinterface.c` `37463c8f…` and `fakegv.c` `912cc10a…`.

### Environment (executed locally)

- Linux 6.12, glibc 2.41, GCC 14.2.0, Clang 19.1.7, CMake 3.31.6, Ninja,
  Python 3.13.5.
- Aravis 0.8.36 at `/tmp/gm8/aravis-prefix`, uninstrumented, built by
  `scripts/provision_aravis.sh` with packet sockets disabled.
- System GLib 2.84.4.
- `lo` = 127.0.0.1/8. Ordinary user; no capabilities.
- Every SDK-enabled binary resolves `libaravis-0.8.so.0` to
  `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0` (`ldd` and the test's own
  `dladdr` print).
- The adapter is the static `gige_mel_aravis` archive of this commit, linked
  into each test binary of the listed build directory. `ldd` does not show it.

### Results

| Configuration | SDK-free CTest | SDK-enabled CTest | `aravis_gige_loopback_contract` |
| --- | --- | --- | --- |
| GCC dev | **8/8** | **14/14** | 10/10 scenarios, 42.5 s |
| GCC Release | **8/8** | **14/14** | 10/10 |
| Clang 19 dev | **8/8** | **14/14** | 10/10 |
| ASan/UBSan (`detect_leaks=1:halt_on_error=1`, UBSan `halt_on_error=1`) | **8/8** | **14/14** | 10/10, no report |
| TSan (`halt_on_error=1`) | **8/8** | **14/14** | 10/10, with the suppression caveat below |

The SDK-enabled suite takes about 65 s in each configuration. The loopback
test is about 43 s of that; most of it is bounded 1 s SDK discovery polls and
real 25 Hz frame periods. This fits within the 25-minute hosted job timeout.

The following were also executed:

- `vendor_upstream.py --verify` (97 files) and `verify_json.py` (2 files),
  both offline;
- all 29 package-isolation verifier tests;
- `git diff --check`.

The real package-isolation verifier ran on fresh SDK-enabled installs of the
dev, Release and Clang builds, with `PKG_CONFIG_PATH` unset. Each install
passed all six checks and printed "package isolation verified":

- `adapter-sdk`: `aravis_adapter_tests` loads the pinned SDK;
- `provider-deps`: 6 dependencies, none from Aravis/GLib;
- `installed-names`: 157 installed paths;
- `installed-text`: 97 files;
- `installed-symbols`: 2 binaries;
- `consumer`: the separately built consumer.

The SDK-free install's separately built consumer also passed for the same
three builds. The installed file list is identical, path for path, to the
earlier `/tmp/gm8/inst-ar` SDK-enabled install: no loopback, simulator or test
file is installed. `gige_mel_aravis_diagnostic --help` passed in every build.

### Network evidence (from the executed GCC dev run)

- **Simulator.** `arv_gv_fake_camera_new("127.0.0.1", "GVTest")` started
  (`is_running`) with an `ArvFakeCamera` underneath. UDP 127.0.0.1:3956 was
  free before start and `EADDRINUSE` after it.
- **Discovery.** Restricted to `lo`, exactly one device was found:
  `Aravis-Fake-GVTest`, "Aravis Fake serial GVTest (GigEVision 127.0.0.1)".
  Device address 127.0.0.1:3956; native types `ArvGvDevice`/`ArvGvStream`.
- **Real GVCP.** The server's CCP was nonzero while the client was open and 0
  after close. A server-memory nonce (`0xC0FFEE01`, then `…02`) was read back
  through the client's `TestRegister` feature. Configuration writes appeared in
  the server's registers. The acquisition register went 0 → 1 → 0 on
  `AcquisitionStart`/`Stop`.
- **Real GVSP.** The client programmed SCDA0 to 127.0.0.1 and SCP0 to a port
  owned by this process (for example 46871); the server's packet size was 1 400.
  Eight frames arrived as 40 received GVSP packets in 8 completed SDK buffers
  (leader, 3 payload, trailer each), with 0 failures and 0 missing packets.
  Every one of the 3 072 pixels per frame matched `(x + y + id) % 255`. Native
  IDs started at 65 401, the pinned initial counter. After stop, SCP0 was 0 and
  the client's UDP socket was closed. Throughout, every UDP socket the process
  owned was on 127.0.0.1 or 255.255.255.255.
- **Lifetime.** `request_stop`, `stop`, an exception unwind and `close` each
  returned a blocked network receive in 4 ms. The stream was finalized before
  the device. A held lease's bytes stayed unchanged after stop and close. An
  old lease released during a new run freed its buffer and did not requeue
  it (the pool stayed at 4).
- **Diagnostics.** `capture_one()` wrote `P5\n64 48\n255\n` plus 3 072 bytes,
  3 085 in total, with every pixel matching. One `time_probe()` frame line, as
  executed:

  ```text
  {"record":"frame","index":0,"device_id":"Aravis-Fake-GVTest","interface":"gige_vision",…,
   "stream_generation":13,"native_frame_id":65401,"width":64,"height":48,"offset_x":8,"offset_y":4,
   "sdk_buffer_timestamp_ns":1793556764265116000,"sdk_system_timestamp_ns":1793556764265195000,
   "actual_stream_timestamp_path":"unverified","descriptor_timestamp_valid":false,"timestamp_qualified":false,…}
  ```

  The header had `diagnostic_tick_frequency_hz` 1 000 000 000, equal to the
  server's bootstrap registers. That still does not prove the stream's own
  read. The GenICam XML SHA-256 and size equalled those the simulator serves.
  The buffer timestamp is a conversion of the simulator's host-wall-clock
  leader value. It is **not** integration start.
- **MEL.** With the test-only clock, 5 network frames (IDs 65401…65409) were
  published. Each destination equalled the SDK lease bytes and the oracle, and
  `systemTime` was the declared virtual value (5 000 000 000 000 ns, …). Four
  frames were dropped while all four destinations were held, and held bytes
  were unchanged. Without the clock (control), 6 genuine GVSP frames were
  rejected with "timestamp contract", with 0 callbacks and sentinel
  destinations unchanged.
- **Simulated loss.** At `gvsp-lost-ratio` 0.0: 4 frames from 20 packets. At
  1.0: 0 packets and 0 frames received, 3 bounded 400 ms timeouts, a prompt
  cancel, and the pool held at 4 buffers. Through the provider, nothing was
  published and destinations were unchanged.
- **Failure diagnostics (control).** With 127.0.0.1:3956 held by another
  socket, the test failed **immediately** with "UDP 127.0.0.1:3956 (GVCP) cannot
  be bound before the simulator starts: Address already in use …. Not
  skipped." It did not skip and did not fall back to the in-process Fake.

### Focused repeats

Each focused scenario ran three times with `FILTER 3`, in one process per
filter, after the matrix:

| Build | A | C | D1 | D2 | M1 | M2 | L |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GCC dev | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 (+3) | 3/3 | 3/3 |
| TSan (`halt_on_error=1`, committed suppression file) | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 (+3) | 3/3 | 3/3 |

The substring filter `"L "` also matches "M1 … MEL publication", so M1 ran
three further times inside the L process. That gives 6 passes in that log and
6 M1 runs in total.

### TSan: investigation and the loopback-only suppression file

**Reproduction.** The same TSan objects were relinked locally into a control
binary that used the existing `tsan_uninstrumented_sdk.cpp`, with only the two
patterns, instead of the new file. One full run with `halt_on_error=0` passed
all 10 scenarios functionally but printed **68** reports (`rc=66`). Each one
was classified by its two access stacks:

| Count | Side 1 | Side 2 | Meaning |
| --- | --- | --- | --- |
| 20 | `recv`/`poll` from `libgio`/`libaravis` (`_send_cmd_and_receive_ack`), heartbeat thread | the same calls on the caller's GVCP feature I/O | `ArvGvDevice` GVCP buffer and poll fd, both under `io_data->mutex` (pinned `src/arvgvdevice.c:138-180`). SDK-internal on both sides |
| 35 | `write`/`close` from `libglib`, reached from the project's `unique_ptr::reset` of the last SDK reference (`retire_locked()`/`close()`) | `eventfd` from `libglib` on the heartbeat/receiver thread | GCancellable descriptors during SDK finalization (`g_cancellable_cancel` versus `make_pollfd`/`release_fd`), serialized by GCancellable's GMutex and `g_thread_join`. SDK-internal on both sides |
| 13 | **project** read of leased bytes after `arv_stream_timeout_pop_buffer()`: test `pixels_match`/`inspect`, production `write_pgm`, and the provider's destination `memcpy` | `memcpy` from `libaravis` `_loop` on the `arv_gv_stream` thread (pinned `src/arvgvstream.c:577`) | **The SDK buffer-handoff boundary.** The buffer reaches the caller through `GAsyncQueue` (a GMutex) only after completion, and the SDK ignores packets for a completed frame. This is the same boundary the two GIGE-MEL-009 patterns cover for the Fake stream; on GigE the writer is `memcpy`, so they do not match |

On Linux a GMutex is an inline futex that TSan cannot see. No report has a
project access on **both** sides. These reports were investigated, not
discovered defects. They do not prove that the SDK boundary is race-free.

**Decision.** The shared file and its two patterns are unchanged for the
three earlier executables. The loopback executable alone links
`tests/support/tsan_gige_loopback_sdk.cpp`. It keeps the same two patterns
and adds `called_from_lib:` for `libaravis-0.8.so.0`, `libglib-2.0.so.0`,
`libgio-2.0.so.0` and `libgobject-2.0.so.0`. This is not a whole-report `race:`
pattern. TSan ignores the memory accesses and synchronization of intercepted
calls made from those four libraries.

**What it costs (stated as a limitation).** For this executable, no race in
which one side is such an SDK-internal libc access is reported, including the
SDK writing a buffer the project reads (group 3). That blind spot is
**wider** than the two existing patterns. A green TSan run of
`aravis_gige_loopback_contract` is therefore not race coverage of the GigE
buffer-handoff boundary, and it is not instrumented-SDK coverage. That
boundary is asserted functionally instead: held-lease bytes are unchanged
after stop, while held and across reopen; every pixel is checked; and a
retired buffer is never requeued.

**Controls (executed).**

- An intentional project-only data race, linked with
  `tsan_gige_loopback_sdk.cpp`, still halted with `halt_on_error=1` (`rc=66`).
- The probe of the same GigE cycle with the two existing patterns
  reproducibly gave 6 reports in each of 3 runs; with the four
  `called_from_lib` entries added it gave 0 in each of 3 runs.

**Executed result.** With the committed suppression file and
`TSAN_OPTIONS=halt_on_error=1`, the full TSan SDK-enabled suite passed 14/14,
including the loopback test. It passed again after the final comment-only
rebuild.

### What this does not establish

- No physical camera, firmware, NIC, switch, MTU/jumbo frame, firewall,
  multi-interface host or real packet loss/reordering is qualified. The pixels
  are the simulator's synthetic ramp.
- The controller privilege, heartbeat and stream-channel behavior observed
  are the pinned simulator's own implementation.
- Packet resend is not exercised: the simulator advertises no resend
  capability, and scenario C asserts this. No exact lost-packet or
  resend-event count is claimed.
- A timeout under simulated loss is not evidence of disconnection.
- No exposure timing, integration start, PTP or UTC is established. The GVSP
  leader timestamp derives from the simulator's host wall clock. The
  diagnostic reports the GigE path as `"unverified"`. Production descriptors
  stay `FrameTimestamp{}`.
- Positive MEL publication used the **test-only** virtual clock. Production
  Aravis-backed MEL Image publication is still not enabled.
- Hosted CI results are recorded in the PR, not here. Local execution does not
  imply a GitHub Actions result.

## GIGE-MEL-010-R1 review corrections (2026-11-01)

**Starting point.** This is one ordinary corrective commit on
`feature/gige-mel-010-timestamp-provenance` (PR #10). Before editing, the
following were freshly fetched and agreed:

| Item | Commit | Tree |
| --- | --- | --- |
| Local HEAD, remote branch and PR head (the reviewed head) | `c6d61efff6aac70a1d43cdaf43aaface05a96251` | `a592fea7e87eefae29292f48c9e0fee88eeeb032` |
| Main (not advanced; PR base) | `214e4477709096fbdb2800ba824bd20c05172592` | `5f7e712998676e2d96a0e051287f3018a6ab7798` |

The worktree and index were clean. The PR was open, not a draft, `MERGEABLE`
/`CLEAN`, with auto-merge off. There was no intervening work.

**Scope.** The following changed:

- `src/backends/aravis/aravis_time_probe.cpp`;
- the private `aravis_backend.hpp`: it declares the production serializer
  `render_time_probe_report()`/`time_probe_json_string()`, the schema
  version constant, and the test-only `time_probe_with_faults_for_testing()`
  checkpoint seam;
- `tests/aravis_timestamp_contract.cpp`;
- `docs/aravis-integration.md` and this record.

The following are unchanged:

- `include/gige_mel/*` and `src/provider.cpp`;
- `FrameTimestamp{}` in the production Aravis descriptor, and source-lease
  ownership (`aravis_backend.cpp` is untouched);
- the virtual-clock fixture (`tests/support/*`);
- `tools/aravis_diagnostic.cpp`. It still calls only `time_probe()`, so the
  fault seam is unreachable from the CLI;
- factories and `getAPI_Manager()`;
- CMake, the workflow, presets, sanitizer options and the two TSan
  suppressions;
- the SDK pin and provisioning, the manifests, the verifier and its 29
  tests, and Python 3.9.

No CTest target was added: there are still **13** SDK-enabled entries and
**8** SDK-free.

### Corrections

| Finding | Correction |
| --- | --- |
| A. The GigE `sdk_buffer_timestamp_source` named a definite path ("SDK conversion of the GVSP leader timestamp …" or "… substitutes its host reception wall-clock time") from the diagnostic's own pre-stream frequency read | **Schema version 2.** `diagnostic_tick_frequency_hz` keeps the observed value, and `diagnostic_tick_frequency_read` says it is a separate read before stream creation, not proof of the value the SDK stream sampled at `src/arvgvstream.c:1613` (where the error is ignored). `actual_stream_timestamp_path` is `"unverified"` for GigE in the header and every frame. `sdk_stream_timestamp_paths` states both conditional SDK behaviors from the pinned source. The Fake statement (`pinned_fake_fill_host_wall_clock`) is kept, because it is established by source. A GigE context without a read result, or a frequency for a non-GigE device, is refused rather than rendered. |
| B. Every byte ≥ 0x7F was written as `\u00XX`, so `Cámara – König` decoded as `CÃ¡mara â\u0080\u0093 KÃ¶nig` | A strict Unicode Table 3-7 validator and encoder. Valid UTF-8 is copied exactly. Only `"`, `\`, U+0000-U+001F (including NUL) and DEL are escaped. Malformed input is an `io_error` that names the field (for example `'header.identity.DeviceModelName'`), with offset and bytes. The output is removed, and nothing is substituted. A malformed `DEVICE_ID` is `invalid_argument` before any output or device I/O. |
| C. `backend.open()` and the post-`try` rendering and writing ran outside the exception handler, so a throw left the `"wx"` output behind | `ExclusiveOutput` (RAII): it owns only a path this call created, and removes it on every non-committed exit, including unwinding. It closes exactly once, never throws, and never touches a file it failed to create. `NativeSession` (RAII): it owns the device and the stream from creation, and runs the established `stop()`, stream destruction and `close()` exactly once. One handler covers open through commit. The first failure is kept in `error`, and a close failure stays in `cleanup_error`. There is no second native stop and no `arv_shutdown()`. The start/stop/close order and the single deadline are unchanged. |

> **Superseded current claims** (the GIGE-MEL-010 record below is kept
> unchanged as history):
>
> - schema version 1 and its `tick_frequency_hz`/`tick_frequency_source`
>   fields;
> - "For GigE, `sdk_buffer_timestamp_source` also states whether the tick
>   frequency read at probe time was zero" (it identified a path);
> - "All strings are JSON-escaped (control bytes, DEL and bytes ≥ 0x80 as
>   `\u00XX`)";
> - O3's "`tick_frequency_hz` was `null`". It is now
>   `diagnostic_tick_frequency_hz`, and that is still `null` for the Fake.

### Before/after controls

The controls are **source-level** for A and B and **fixture-level** for C.
They are not a real-camera or physical GigE reproduction.

**Reviewed production.** It was checked out in a disposable detached
worktree, `/tmp/gm10r1/wt-reviewed`:

- HEAD `c6d61ef…`, tree `a592fea7…`, 0 changed files;
- `aravis_time_probe.cpp`: blob `ea39274d…`, SHA-256 `2aaac646…f56448`;
- `aravis_backend.hpp`: blob `8ca76f5c…`.

It was built SDK-enabled (GCC 14.2.0 Debug) into `/tmp/gm10r1/build-reviewed`,
and its own `aravis_timestamp_contract` passed: 12 scenarios.

**Control program.** One program, `/tmp/gm10r1/controls/control.cpp`, applies
the same expectations to both versions:

- `-DREVIEWED` includes the reviewed TU to reach its anonymous-namespace
  `render()`/`quote()`, and links the reviewed adapter archive;
- otherwise it uses the corrected public private-header functions and the
  corrected archive.

Both executables load `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`
(0.8.36) and glibc 2.41. No control file or JSONL is in the repository.

| Control | Fixture | Reviewed `c6d61ef` | Corrected |
| --- | --- | --- | --- |
| A | GigE `TimingContext`, diagnostic read 1 000 000 000 Hz | **FAIL:** `sdk_buffer_timestamp_source` = "SDK conversion of the GVSP leader timestamp with the stream's tick frequency (src/arvgvstream.c:480-482) …"; no `actual_stream_timestamp_path` | PASS: "conditional on the tick frequency the SDK stream read during its own construction … unverified"; `actual_stream_timestamp_path` = `"unverified"` |
| B | `Cámara – König` (18 bytes), decoded with nlohmann/json | **FAIL:** encoded `"C\u00c3\u00a1mara \u00e2\u0080\u0093 K\u00c3\u00b6nig"`, decoded `CÃ¡mara â KÃ¶nig` (25 bytes) | PASS: encoded `"Cámara – König"`; decoded exact (18 bytes) |
| C | Non-Aravis backend whose `open()` throws `std::runtime_error`, via the production `time_probe()` | **FAIL:** the exception **escaped** `time_probe()`; output file **left behind** | PASS: returned "time probe failed: control: injected backend.open() exception; the output was removed"; file absent |

The reviewed program exited 1 with 3 failures, and the corrected one exited 0.
Each failure is the reviewed defect itself. Setup, headers and the SDK
loaded identically in both builds.

### Permanent regressions and counts

`aravis_timestamp_contract` grows from 12 to **17** scenarios. The 12
existing scenarios are unchanged except O3, which asserts schema version 2,
exactly one header/6 frames/one summary, and the new Fake provenance fields.

| Scenario | Covers |
| --- | --- |
| P1 | A1 (1 GHz and `UINT64_MAX`), A2 (0), A3 (no read result: refused), A4 (Fake; a Fake or other device with a frequency is refused). Production serializer, synthetic `TimingContext`. GigE headers are identical apart from the number |
| P2 | B1-B5 plus range boundaries and the empty string: exact decode. B6: 15 malformed forms, each with its reason. Field naming through the full report. Malformed `DEVICE_ID` refused before I/O |
| P3 | A real SDK identity (`DeviceModelName` written to Fake memory `0x68`): `Cámara – König 📷` is SDK value == parsed JSONL value. A malformed value fails with the field named, after cleanup, with the output removed |
| P4 | C1 (existing bytes, including NUL, unchanged; 0 opens), C2, C3, C4 (scripted throwing `open()`; also a throw inside the adapter's `open()`, after which the reservation is released), C5 (checkpoint throws at `opened`/`started`/`render`/`written`) |
| P5 | C6: post-start timeout keeps the first error; stream, pool and device retired; output removed; an SDK-rejected `AcquisitionStop` reported separately in `cleanup_error` |

The C5 seam (`TimeProbeTestFaults::checkpoint`) is declared in the private
`aravis_backend.hpp`:

- It can only throw at named boundaries of the same production routine.
- It cannot change native camera behavior.
- The diagnostic does not call it: `tools/aravis_diagnostic.cpp` is
  unchanged and calls only `time_probe()`.

C6 uses the existing documented decoded-facts seam, which does not modify
the SDK buffer, and an SDK-arranged imposed RO access mode.

### Local results (executed)

The environment is as in the GIGE-MEL-010 record:

- Linux 6.12, GCC 14.2.0, Clang 19.1.7, CMake 3.31.6, Ninja, Python 3.13.5;
- Aravis 0.8.36 at `/tmp/gm8/aravis-prefix`, uninstrumented;
- `PKG_CONFIG_PATH` set to the prefix plus the sysroot GLib directory.

The pinned citations were rechecked: `src/arvgvstream.c` and
`src/arvfakecamera.c` from commit `97ff5e33…` are byte-identical (SHA-256
`8ef52e1d…`, `eb63937a…`) to the checksum-verified archive.

| Configuration | CTest | `aravis_timestamp_contract` |
| --- | --- | --- |
| GCC dev + Aravis | **13/13** | 17/17 |
| GCC Release + Aravis | **13/13** | 17/17 |
| Clang 19 + Aravis | **13/13** | 17/17 |
| ASan/UBSan + Aravis (`detect_leaks=1:halt_on_error=1`, `halt_on_error=1:print_stacktrace=1`) | **13/13** | 17/17 |
| TSan + Aravis (`halt_on_error=1`, the two unchanged whole-report suppressions) | **13/13** | 17/17 |
| GCC dev, SDK-free | **8/8** | — |

Other results:

- Existing suites (dev): `aravis_adapter` 22/22, `aravis_stream_contract`
  15/15, the negative `aravis_mel_embedding` probe PASS, provider 74/74.
- Within `aravis_timestamp_contract`:
  - V0-V3: positive virtual-clock MEL publication;
  - N1-N4: negative admission;
  - C: with the clock removed, the same genuine frames are rejected.

  All of these pass unchanged.
- Bounded repetition of the new paths:
  - dev: P2, P3, P4 and P5 ×10 each;
  - ASan/UBSan and TSan: P3, P4 and P5 ×5 each.

  All passed.
- Verifiers: `vendor_upstream.py --verify` (97 files), `verify_json.py` (2
  files), and the verifier suite 29/29.
- `verify_package_isolation.py`: "package isolation verified" for the dev,
  Release and Clang SDK-enabled installs, with `PKG_CONFIG_PATH` unset. The
  separate consumer configured, built and ran.
- The SDK-free install has the same non-documentation file set as the
  SDK-enabled install. Its consumer, configured with `PATH=/usr/bin:/bin` and
  no `PKG_CONFIG_PATH`, passed.
- With no SDK discoverable, `-DGIGE_MEL_BUILD_ARAVIS=ON` fails with
  "requires exactly Aravis 0.8.36".
- Diagnostic:
  - `--help` exits 0;
  - `time-probe` with COUNT 0 exits 2;
  - production GigE-only `time-probe Fake_1 3 …` exits 1 ("not found"), and
    the output is removed;
  - an existing output exits 1 and is unchanged.
- `git diff --check`: clean.

The TSan result remains subject to the documented whole-report suppression
blind spot for the uninstrumented SDK and GLib. It is not unrestricted race
coverage.

Hosted CI for this commit is recorded in the PR, not here.

### Remaining limitations

- No physical camera or GigE network was used.
- The GigE provenance rows are exercised only through synthetic
  `TimingContext` values passed to the production serializer. The
  in-process Fake does not construct an `ArvGvStream`, and no lost-frequency
  event was reproduced.
- The stream's actual GigE path stays **unverified** by design. Fixing it
  needs a camera timing profile (checklist item 13 in
  [aravis-integration.md](aravis-integration.md#camera-timing-qualification-checklist)).
- No integration-start, epoch, UTC, PTP, continuity or accuracy claim is
  made. Every record keeps `timestamp_qualified=false`, and the production
  descriptor stays `FrameTimestamp{}`.
- Every item of the camera-timing qualification checklist remains open.

## GIGE-MEL-010 native timestamp provenance and virtual-clock MEL publication (2026-11-01)

**Starting point.** The branch `feature/gige-mel-010-timestamp-provenance` was
created from freshly fetched `origin/main`. Main had not advanced since task
preparation:

- commit `214e4477709096fbdb2800ba824bd20c05172592`, tree
  `5f7e712998676e2d96a0e051287f3018a6ab7798`;
- the merge of PR #9, whose second parent is the reviewed head `698b1706…`.

The worktree was clean. All GIGE-MEL-008 and GIGE-MEL-009(-R1) corrections are
preserved.

**Scope.** The following changed:

- Adapter (private, uninstalled):
  - `aravis_backend.{hpp,cpp}`: the read-only observation path
    `receive_with_native_timestamps()` and `read_timing_context()`. The
    production `receive()` is now the same body with no observation output.
  - New `aravis_time_probe.cpp`: the shared `time_probe()` routine.
- Diagnostic: the `time-probe` command.
- Tests:
  - new `tests/aravis_timestamp_contract.cpp`;
  - new test-only `tests/support/aravis_virtual_clock.*`;
  - the TSan support file comment.
- Build: `cmake/GigeMelAravis.cmake` gains one target and one CTest entry.
- Documentation.

The following are unchanged:

- `include/gige_mel/*` (`backend.hpp`, `provider.hpp`), `src/provider.cpp`
  and its timestamp admission;
- `FrameTimestamp{}` for every Aravis frame;
- the acquisition and recovery state machine, source lease ownership, and
  Mono8 validation;
- factories and `getAPI_Manager()`;
- pins and manifests, Aravis 0.8.36 and its provisioning, the default-OFF
  option;
- the workflow and its five lanes, presets, sanitizer options, and the two
  TSan suppression patterns;
- the verifier and its 29 tests, and Python 3.9.

No timestamp qualification, hardware claim, automatic reconnect, camera, PTP
or network configuration, zero-copy destination, packed format or second SDK
was added.

**Evidence kinds kept separate:**

1. **Real pinned SDK execution:** every test binary loads
   `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`, runtime and compiled
   0.8.36.
2. **Fake device behavior:** in-process `ArvFakeDevice`.
3. **Test-injected virtual timestamps:** `tests/support/aravis_virtual_clock.*`
   only.
4. **Actual MEL callbacks and buffer bytes:** checked inside `onImage`.
5. **Purely source-backed timing claims:** cited
   [pinned source](aravis-integration.md#what-the-pinned-sdk-actually-reports).
   All nine cited files in the release archive have Git blob IDs identical to
   commit `97ff5e33…`.

No physical camera or GigE network was used.

**Environment and SDK.** The environment is unchanged: Linux 6.12, GCC 14.2.0,
Clang 19.1.7, CMake 3.31.6 and Ninja. The SDK is Aravis **0.8.36**, built from
release commit `97ff5e33…` (archive SHA-256 `246deaa0…51bf`) into
`/tmp/gm8/aravis-prefix`, without instrumentation. Every SDK test binary
printed, and `ldd` confirmed, `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`,
runtime and compiled 0.8.36.

| Configuration | CTest | Provider | Backend | Portability | `aravis_adapter` | `aravis_stream_contract` | `aravis_timestamp_contract` | MEL probe |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| GCC dev, SDK-free | **8/8** | 74/74 | 8/8 | 12 + 2 | — | — | — | — |
| GCC Release, SDK-free | **8/8** | pass | pass | pass | — | — | — | — |
| ASan/UBSan, SDK-free | **8/8** | pass | pass | pass | — | — | — | — |
| TSan, SDK-free | **8/8** | pass | pass | pass | — | — | — | — |
| Clang 19, SDK-free | **8/8** | pass | pass | pass | — | — | — | — |
| GCC dev + Aravis | **13/13** | 74/74 | 8/8 | 12 + 2 | 22/22 | 15/15 | 12/12 | pass |
| GCC Release + Aravis | **13/13** | pass | pass | pass | 22/22 | 15/15 | 12/12 | pass |
| Clang 19 + Aravis | **13/13** | pass | pass | pass | 22/22 | 15/15 | 12/12 | pass |
| ASan/UBSan + Aravis (`detect_leaks=1:halt_on_error=1`) | **13/13** | pass | pass | pass | 22/22 | 15/15 | 12/12 | pass |
| TSan + Aravis (`halt_on_error=1`, the two unchanged patterns) | **13/13** | pass | pass | pass | 22/22 | 15/15 | 12/12 | pass |

The SDK-enabled build adds exactly one CTest entry, `aravis_timestamp_contract`,
making 13 instead of 12. The SDK-free count stays at 8. The existing
scenario-group counts are unchanged: provider 74, backend 8, portability
12 + 2, adapter 22, stream contract 15.

**Focused repetitions.** These are bounded: each scenario has a 120 s
in-process watchdog, every wait is bounded at 10 s, and every run has an outer
`timeout`.

| Configuration | Repetitions |
| --- | --- |
| dev, ASan/UBSan, TSan | N3 (terminal discontinuity and recovery) ×20; V1, V2, V3, N1, N2, N4 ×5 each; the whole contract ×3; the negative embedding probe ×3 |

All passed. The logs contained zero sanitizer reports.

**Defect found by repetition and corrected before commit.** One ASan
repetition of N3 (1 of 5) failed. The test waited for
`stream_failures == 1`, then read `last_error` once. The provider increments
`stream_failures` before it records the message (`src/provider.cpp` `run()`),
so the read could observe the earlier "rejected" text. The test now waits for
both facts together. This was an assertion-ordering race in the new test, not
a provider or adapter defect. After the correction, N3 passed 20/20 in each
of the three configurations.

**Before/after control on the same pinned DSO.** This control is disposable
and was kept outside the source tree in `/tmp/gm10/control`.

Setup:

- A detached worktree of unchanged main (`214e447…`, tree `5f7e712…`) was
  built with the SDK into `/tmp/gm10/base-build`.
- A control program was linked against **main's** `libgige_mel_aravis.a` and
  `libgige_mel_provider.so`, plus the branch's virtual-clock source (SHA-256
  identical to the committed files). Configuration: 64×48@(8,4), 100 Hz,
  10 ms, two `0xA5` destinations, wait for ≥ 10 backend frames.

Results:

| Mode | Provider delivered / rejected | Image callbacks | Pixel oracle / header OK | Destinations |
| --- | --- | --- | --- | --- |
| `plain`: main's production adapter | 0 / 10 | **0** | — | still sentinel; `last_error` "…timestamp contract" |
| `decorated`: + virtual clock | 10 / 0 | **10** | 10 / 10 | written |
| `passthru`: decorator, clock removed | 0 / 10 | **0** | — | still sentinel |

Linked-revision proof:

- Aravis is a **static** adapter archive, so `ldd` alone proves nothing about
  the adapter code. Main's archive holds three objects, and the branch's adds
  `aravis_time_probe.cpp.o`.
- The branch-only diagnostic string "timestamp observation requires a stream
  created by the Aravis adapter" and the symbol
  `aravis::receive_with_native_timestamps` are **absent** from the control
  binary and from main's test binaries.
- The same string and symbol are **present** in every branch SDK test binary
  and the diagnostic.
- All of these binaries load the same `libaravis-0.8.so.0`.

Inside the branch's test, scenario C repeats the pass-through control. That
negative result comes from frames actually received and rejected, not from a
setup failure: every setup step is checked and fails with exit 4.

**Diagnostic and probe evidence:**

- `--help`: exit 0.
- `time-probe` with a missing argument, COUNT 0/33/`1e1`/`-1`, or an empty ID:
  exit 2.
- Production GigE-only `time-probe Fake_1 3 …`: exit 1, "not found among 0
  discovered device(s)". There is no Fake fallback, and no output file is
  left behind.
- An existing output file: exit 1, "no device I/O was performed", and the
  file is unchanged.
- The `capture` regression (stream contract J) passes, and `capture` with a
  missing argument exits 2.
- The actual probe against the private Fake ran through the same
  `time_probe()` routine (scenario O3). It wrote 8 valid JSONL lines;
  `tick_frequency_hz` was `null`; raw values were inside the wall-clock
  window; no write; only Start/Stop.
- Raw observation sample (O1): frames 65401–65404, generation 1,
  `sdk_buffer_timestamp_ns == sdk_system_timestamp_ns`, about 1.79 × 10¹⁸
  (Unix-epoch wall clock), 10 ms apart at 100 Hz, descriptor timestamp
  invalid.
- Fake GenICam XML: 15 983 bytes, SHA-256 `bfd8b883…b00b`.

**Package and verifiers:**

- `vendor_upstream.py --verify`: 97 files.
- `verify_json.py`: 2 files.
- Verifier regression suite: 29/29 under the local Python 3.13, and 29/29
  under CPython **3.9.22** (`docker.io/library/python:3.9.22-slim-bookworm`,
  read-only mount, non-root, no network).
- `verify_package_isolation.py`: "package isolation verified" for the dev,
  Release and Clang SDK-enabled installs, with `PKG_CONFIG_PATH` unset.
- SDK-free and SDK-enabled installs have identical non-documentation file
  sets.
- Exported dynamic symbols, `NEEDED` entries and installed headers are
  identical to main's install.

  A raw grep of the Debug installs finds `aravis` only in the build-directory
  path (`build/aravis-dev`) embedded in debug information. The Release install
  has 0 matches.
- The separate installed consumer configured, built and ran without SDK
  discovery (PASS).
- With no SDK discoverable, the default configure is OFF and passes 8/8, and
  `-DGIGE_MEL_BUILD_ARAVIS=ON` fails with "requires exactly Aravis 0.8.36".
- The synthetic native-loader `mel_capture` wrote `P5 64 48`.
- `git diff --check` against main: clean.

**What this record does not establish.** The virtual-clock passes are
test-qualified MEL publication of genuine Aravis pixels under **declared test
time**. They are **not** hardware or Aravis timestamp qualification. The
following remain open:

- the Fake's exposure timing;
- GVSP or physical-camera behavior;
- every item of the
  [camera-timing qualification checklist](aravis-integration.md#camera-timing-qualification-checklist).

The SDK remains uninstrumented, and the documented TSan whole-report blind
spot now also applies to `aravis_timestamp_contract_tests`.

Hosted CI is recorded in the PR handoff, not here, because its results come
after this commit.

## GIGE-MEL-009-R1 review corrections (2026-11-01)

**Starting point.** This is one ordinary corrective commit on
`feature/gige-mel-009-aravis-stream` (PR #9). Before editing, the following
were freshly fetched and agreed:

| Item | Commit | Tree |
| --- | --- | --- |
| Local HEAD, remote branch and PR head (the reviewed head) | `c333a2af90a5c303f4d169eff7ed82967b5731ba` | `8a06d13fd39d0c4eb0710e088eabd9e4c4254ce7` |
| Main (not advanced) | `ae8de93c6749378e7b4254935bd3ce7cf08cbe06` | `9d6fd135bd96037f5e6cd7404c3ee5b39549ac58` |

The worktree was clean and there was no intervening work, so all five
findings applied as reviewed.

**Scope.** The following changed:

- the private adapter (`aravis_backend.cpp`), with a new test-only
  `TestHooks::command_result` in `aravis_private.hpp`;
- the profile helpers (`camera_profile.*`, adding `acquisition_preflight`);
- three Aravis tests and the TSan support comment;
- documentation.

The following are unchanged:

- `include/gige_mel/*` and `src/provider.cpp`;
- factories and `getAPI_Manager()`;
- `FrameTimestamp{}` and provider timestamp admission;
- pins and manifests;
- Aravis 0.8.36 and its provisioning;
- the default-OFF option;
- the workflow, the five lanes, presets and sanitizer options;
- the two suppression patterns;
- the verifier and its 29 tests;
- the two deferred PR #3 notes.

No public recovery API, generic GenICam API, automatic restart or reconnect,
timestamp qualification or hardware claim was added.

### Corrections

| Finding | Correction |
| --- | --- |
| A. A failed `AcquisitionStart` was treated as proof the camera never started | `uncertain` is recorded before the SDK executor runs. After an attempted start, the original error is kept, one compensating `AcquisitionStop` is made, and the partial stream and pool are retired. If that stop fails, the outcome stays uncertain and owned. Pre-execution failures invent no uncertainty. |
| B. A failed `AcquisitionStop` left configuration and restart permitted | `apply()` and `start()` refuse an uncertain outcome before writes, streams, pools or commands. The owning wrapper's later `stop()` makes one bounded recovery attempt. `close()` attempts a stop with no native stream, and its diagnostics say "recovered" or "remains uncertain" truthfully. |
| C. Acquisition skipped fresh-limit and current-ROI validation | A read-only `acquisition_preflight()` reuses preconditions, `check_limits`, `check_current`, the `apply()` envelope comparison and `configuration_from()` before any acquisition side effect. |
| D. Two Health assertions could select a stale captured batch | Payload predicates (`running`/`idle` plus valid configuration) replace `[](auto&){return true;}`. A forced stale-batch schedule is now permanent for both directions. |
| E. TSan qualification overstated coverage | The comment and documents now state the whole-report suppression blind spot. The patterns are unchanged. |
| 8A. `cancel_blocked()` cleanup | A shared `ReceiverCleanup` (request_stop, then join, then stop) is installed before start. It is also used by scenario I, and exercised by a throw while receive is blocked. |
| 8B. "nothing is allocated per frame" | Now reads "no additional pixel buffers are allocated after pool setup". The per-lease owner record is still allocated. |

The precise contract is described in
[Acquisition-command outcome and recovery](aravis-integration.md#acquisition-command-outcome-and-recovery-gige-mel-009-r1).

> **Superseded current claims** (earlier records are kept unchanged as history):
> "a failed start never requires `AcquisitionStop`"; "no active receiver
> implies configuration/restart is safe" (the stop failure was only reported
> by `close()`); "project code stays fully checked" despite the TSan
> suppressions; "nothing is allocated per frame".

### Before/after controls

The unchanged reviewed production was built once in a disposable detached
worktree, `/tmp/gm9r1/wt-reviewed`, at `c333a2a` with 0 changed files, into
`/tmp/gm9r1/build-reviewed`. No control variant was committed.

**Production corrections (A, B, C), real SDK Fake.** One source,
`/tmp/gm9r1/control/lifecycle_control.cpp`, uses only `TestHooks` fields
present in both revisions, and its failures are SDK-arranged (imposed
read-only access). It was compiled against each revision's **own** private
headers and static archives:

- reviewed `aravis_private.hpp` `35e7a17b…`, `libgige_mel_aravis.a`
  `9f565c11…`;
- corrected header `242dd038…`.

The `acquisition_preflight` symbol appears 0 times in the reviewed binary and
1 time in the corrected one. Neither binary loads a `gige_mel` DSO. `ldd`
resolves the SDK separately to `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`
in both.

| Case | Reviewed `c333a2a` | Corrected |
| --- | --- | --- |
| A. `AcquisitionStart` executed and rejected by the SDK | commands `[AcquisitionStart]`: no compensating stop — **FAIL** | `[AcquisitionStart, AcquisitionStop]` — PASS |
| B. Stop rejected by the SDK (register 1), lease released, stop writable again | `apply` SUCCESS with 2 writes; `start` SUCCESS with 1 new stream — **FAIL** | `apply` refused with 0 writes; `start` refused with 0 streams ("the outcome of AcquisitionStop (stream generation 2) is uncertain …"); the owner's `stop()` then recovers (register 0) — PASS |
| C. SDK readback Height 512, OffsetY 1600 (sensor 2048) | `start` SUCCESS: 1 stream, 4 buffers, 1 command — **FAIL** | refused: 0 / 0 / 0, "OffsetY reads back 1600 with Height 512, outside the sensor extent 2048 …" — PASS |

The reviewed binary exits 1 ("3 control expectation(s) failed"); the
corrected binary exits 0. The failures are the reviewed defects: there are no
setup, header or loader failures.

**Test correction (D): Health selectors against unchanged production.**

- The corrected `aravis_embedding_probe.cpp` was built in a second disposable
  worktree, `/tmp/gm9r1/wt-health` at `c333a2a`, where only that test file
  differs. It links `/tmp/gm9r1/build-health/libgige_mel_provider.so.0` and
  the same SDK.
- It passed 3/3. In both directions the forced schedule delivered the stale
  report at the fixed baseline index, carrying the old state (`idle` after
  enable, `running` after disable). The old unconditional selector accepted
  it; the corrected predicate rejected it.
- Diagnostic variant: with only the post-enable selector swapped back to the
  old one, the probe failed 3/3 at its next assertion, "`acquisition_state ==
  running`". That is exactly the reviewed hazard.
- The reviewed probe itself (old selectors, no forced schedule) passes 3/3
  under the ordinary schedule, which is why the defect was latent.

**Test correction (8A): join-only versus cancellation-first cleanup.** This
was a source-level variant in a disposable copy of the corrected tree
(`/tmp/gm9r1/cleanup-diag`), not a real-SDK reproduction of a production
defect.

- With `JOIN_ONLY=1`, `ReceiverCleanup` only joins. A recorded 3 s watchdog
  keeps it bounded, and its activation fails the control.
- Scenario D then fails with "WATCHDOG fired: cleanup did not release the
  blocked receive", in 3.2 s.
- The committed cancellation-first cleanup passes D, including 3 thrown
  repeats, in 0.7 s. The watchdog never counts as successful cleanup.

**Qualification correction (E): suppression semantics.** These are
`/tmp/gm9r1/tsan/suppression_semantics.cpp` (TSan-instrumented, system GLib)
and the actual SDK reports.

- *Semantics control (a deliberate race, not adapter code).* The test is a
  write/write race between two instrumented project functions. The pattern
  used is `race:g_queue_foreach`, the same mechanism as the two embedded
  patterns.

  | Mode | No suppressions | With the pattern |
  | --- | --- | --- |
  | Racing write called by `g_queue_foreach` | 1 report, exit 66 | **0 reports, exit 0** (the whole project race suppressed) |
  | Plain call, no GLib frame | 1 report | 1 report |

- *Retained unsuppressed reports (GIGE-MEL-009 reviewed run, preserved).*
  The file is `/tmp/gm9r1/evidence/tsan-all.log`, a copy of the original
  `/tmp/gm9/tsan-all.log`. It has 63 reports:
  - 45 with `g_queue_pop_tail` (GLib `free` on the SDK receiver thread);
  - 18 with `memset` under `arv_buffer_set_n_parts`.

  The first access stack of each report has no project frame at #1. In every
  report, the other access is GLib `g_malloc`/`g_malloc0` by the test's main
  thread under the adapter's own locks.
- *R1 focused diagnostic, not an acceptance run.* The corrected contract
  object was relinked **without** `tsan_uninstrumented_sdk.cpp`
  (`/tmp/gm9r1/evidence/contract-tsan-nosupp`, with 0
  `__tsan_default_suppressions` symbols) and run with
  `halt_on_error=0`. Expected findings, labelled separately:
  - 98 reports: 65 `g_queue_pop_tail` and 33 `memset` under
    `arv_buffer_set_n_parts`;
  - 0 reports without a GLib/Aravis frame;
  - 0 whose first access stack has a project frame at #1;
  - every previous access is a GLib allocator call;
  - all 15 scenarios passed (exit 66 only because reports were printed).

  The new K/L/M scenarios appear only as callers of those allocations.
- *Analysis kept from GIGE-MEL-009.* The system GLib's `g_mutex_lock` is an
  inline `lock cmpxchg` futex, which TSan cannot observe, so the correctly
  locked `GAsyncQueue` buffer handoff appears as a race. The patterns are
  therefore kept unchanged. A green run is **not** unrestricted coverage of
  that boundary.

### Permanent regressions and counts

- **`aravis_stream_contract`** grows from 11 to **15** scenario entries: new
  K, L, M and N.
  - D now also runs a fourth, *thrown* canceller three times.
  - G's obsolete assertions were updated. Its blanket "no stop for a failed
    start" became "a compensating stop was issued". B1 then refuses
    configuration with 0 writes and, after the lease is released, refuses
    restart with no stream or command. An explicit `stop()` then recovers
    with no native stream, and `close()` reports the history as recovered.
- **`aravis_adapter`** grows from 21 to **22**: a new SDK-free
  `acquisition_preflight` table. The existing malformed-fact table now also
  runs every case through the preflight.
- **`aravis_mel_embedding`** remains one probe, now with the forced stale
  batch in both directions.
- **New scenarios: 5** (K, L, M, N and the preflight table), plus the thrown
  D variant. No existing scenario was removed.
- **CTest totals are unchanged:** 8 SDK-free and 12 SDK-enabled.

| Required case | Where | Evidence type |
| --- | --- | --- |
| A1. Known pre-execution failure (hook before the executor throws; partial stream and 4 buffers retired; register 0; retry) | K | real Fake |
| A2. Applied start (register observed 1), then an injected error; the compensating stop succeeds; the original `timeout` error is kept; reuse | K | injected command-result fault |
| A3. As A2, but the compensating stop is rejected by the SDK: local retirement; `apply()` with 0 writes; `start()` from both wrappers with no stream, pool or command; recovery by the owner's `stop()` | K | injected + SDK-arranged |
| B1. Successful start, then a rejected stop (register stays 1): configuration and restart are blocked with the lease released; explicit retry | G | SDK-arranged |
| B2. Uncertain, then `close()` with no stream, for a final stop that succeeds and one that fails | L | SDK-arranged |
| B3. Obsolete, never-started and destroyed wrappers; held-lease refusal after recovery; byte validity; history not blocking reuse | M, L | real Fake + SDK-arranged |
| C1. ROI outside the sensor (Height 512, OffsetY 1600, SDK readback) | N | SDK-arranged |
| C2. Invalid explicit `<Inc>0</Inc>` (the SDK returns 0 with no `GError`); restored to 1, then start succeeds | N | SDK-arranged |
| C3. Width increment 1→2, Width 64 → "close and reopen" | N | SDK-arranged |
| Remaining envelope and structural cases | `aravis_adapter` | SDK-free |

In N, every rejected start has zero deltas in streams, buffers, commands and
configuration writes. Native reads do occur. The first N arrangement follows
a completed run, a stop and a lease release.

### Local results (executed)

The environment and SDK are as in the GIGE-MEL-009 record. `PKG_CONFIG_PATH`
is `/tmp/gm8/aravis-prefix/lib/pkgconfig` plus the sysroot GLib directory.

| Configuration | CTest | `aravis_adapter` | `aravis_stream_contract` | MEL probe |
| --- | --- | --- | --- | --- |
| GCC dev + Aravis | **12/12** | 22/22 | 15/15 | pass |
| GCC Release + Aravis | **12/12** | pass | pass | pass |
| Clang 19 + Aravis | **12/12** | pass | pass | pass |
| ASan/UBSan + Aravis (`detect_leaks=1:halt_on_error=1`) | **12/12** | pass | pass | pass |
| TSan + Aravis (`halt_on_error=1`, the two documented patterns) | **12/12** | pass | pass | pass |
| GCC dev, SDK-free | **8/8** | — | — | — |

The SDK-free dev run passes provider 74, backend 8 and portability 12 + 2.

- **Bounded repeats.**
  - TSan: the contract 3× (all 15 scenarios each time, 704 matched
    suppressions per run: 595 `g_queue_pop_tail`, 109
    `arv_buffer_set_n_parts`) and the probe 3×.
  - ASan/UBSan: the contract 3×.
  - Dev: K, L, M and N 3× each, and the probe 3×.

  All passed.
- **Offline verifiers.** `vendor_upstream.py --verify` passes on 97 files and
  `verify_json.py` on 2. The verifier tests pass 29/29.
- **Package isolation.** `verify_package_isolation.py` reports "package
  isolation verified" on real installs from `aravis-dev`, `aravis-release`
  and `aravis-clang`. A separately configured installed consumer, built
  against the `aravis-dev` install with `PKG_CONFIG_PATH` unset, passes.
- **Other checks.**
  - Synthetic capture writes `P5 64 48`.
  - The diagnostic's `--help` exits 0; `capture` with no arguments exits 2.
  - Direct capture and negative MEL admission pass (scenario J and the
    probe).
- **Whitespace.** `git diff --check` is clean over the corrective range and
  the full PR range.

**Not run locally.** The SDK-free Release, ASan, TSan and Clang lanes were
not rerun, because the SDK-free build contains no changed file. The hosted
matrix runs them.

### Remaining limitations

- The applied-then-error cases use an injected command result. There is no
  physical-camera, GVSP, packet-loss or lost-acknowledgment evidence.
- The adapter records command outcomes only. It does not measure camera
  exposure or readout, and has no persistent cross-session recovery registry.
  A reopen is not evidence that an earlier failed stop was resolved.
- The TSan patterns leave the reporting blind spot described above. No
  instrumented SDK/GLib build was made.
- There is still no timestamp, PTP/UTC, successful Aravis MEL Image
  publication, hardware or conformance qualification.

## GIGE-MEL-009 Aravis Mono8 source acquisition (2026-11-01)

**Starting point.** Branch `feature/gige-mel-009-aravis-stream` was created
from freshly fetched `origin/main`. Main had not advanced since task
preparation:

- commit `ae8de93c6749378e7b4254935bd3ce7cf08cbe06`, tree
  `9d6fd135bd96037f5e6cd7404c3ee5b39549ac58`;
- the merge of PR #8, whose second parent is the reviewed R4 head
  `e725461b…`.

The local worktree was clean.

**Scope.** The change covers the adapter (`src/backends/aravis/`, including the
new `aravis_capture.cpp`), the diagnostic, the Aravis CMake fragment, the
Aravis tests (new `tests/aravis_stream_contract.cpp` and
`tests/support/tsan_uninstrumented_sdk.cpp`), and documentation. The
following are unchanged:

- `include/gige_mel/*`, `src/provider.cpp` and its timestamp admission;
- factories and `getAPI_Manager()`;
- the MEL/JSON pins and manifests;
- provisioning, the default-OFF option, the workflow and its five lanes,
  presets and sanitizer options;
- the verifier and its 29 tests.

The design and the evidence are described in
[Source acquisition (GIGE-MEL-009)](aravis-integration.md#source-acquisition-gige-mel-009).

**Environment and SDK.** The environment is the same as in the GIGE-MEL-008
record below: Linux 6.12, GCC 14.2.0, Clang 19.1.7, CMake 3.31.6 and Ninja.
The SDK is Aravis **0.8.36**, built from release commit
`97ff5e330919d7a44b871df4eda669feeafdf342` (archive SHA-256 `246deaa0…51bf`,
re-verified) into `/tmp/gm8/aravis-prefix`, without sanitizer
instrumentation. Every test binary printed, and `ldd` confirmed, the loaded
library `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`, runtime and compiled
version 0.8.36. No camera or network discovery was used.

| Configuration | CTest | Provider | Backend | Portability | `aravis_adapter` | `aravis_stream_contract` | MEL probe |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GCC dev, SDK-free | **8/8** | 74/74 | 8/8 | 12 + 2 | — | — | — |
| GCC Release, SDK-free | **8/8** | pass | pass | pass | — | — | — |
| ASan/UBSan, SDK-free | **8/8** | pass | pass | pass | — | — | — |
| TSan, SDK-free | **8/8** | pass | pass | pass | — | — | — |
| Clang 19, SDK-free | **8/8** | pass | pass | pass | — | — | — |
| GCC dev + Aravis | **12/12** | 74/74 | 8/8 | 12 + 2 | 21/21 | 11/11 | pass |
| GCC Release + Aravis | **12/12** | pass | pass | pass | 21/21 | 11/11 | pass |
| Clang 19 + Aravis | **12/12** | pass | pass | pass | 21/21 | 11/11 | pass |
| ASan/UBSan + Aravis (`detect_leaks=1:halt_on_error=1`) | **12/12** | pass | pass | pass | 21/21 | 11/11 | pass |
| TSan + Aravis (`halt_on_error=1`) | **12/12** | pass | pass | pass | 21/21 | 11/11 | pass |

The SDK-enabled build adds exactly one new CTest entry,
`aravis_stream_contract`, making 12 instead of 11. The SDK-free count stays at
8. `ldd` showed `libasan.so.8` and `libubsan.so.1`, and `libtsan.so.2`, in the
respective test binaries.

**Repetitions.** The new concurrency scenarios were repeated a small bounded
number of times:

- In development, scenario D, cancellation via request_stop/stop/close, runs
  three times per execution.
- Under TSan, the whole contract ran 3 times and the embedding probe 3 times.
- Under ASan/UBSan, the contract ran 3 times.

All runs passed.

**TSan and the uninstrumented SDK.** The first TSan runs reported only races
inside GLib/Aravis queue internals: `g_queue_pop_tail` (546 matches) and
`arv_buffer_set_n_parts` (77 matches). The system GLib's `g_mutex_lock`
disassembles to an inline `lock cmpxchg` futex, which TSan cannot observe. The
two SDK-driving test executables therefore embed
`__tsan_default_suppressions()` for exactly those two functions. `TSAN_OPTIONS`,
presets and lanes were not changed, and project code stays fully checked
*(superseded by GIGE-MEL-009-R1: these whole-report patterns also suppress
reports in which project code participates when a matching SDK/GLib frame is
in a stack)*. A
broader `called_from_lib` variant was tried and rejected, because it caused a
spurious GObject lock-order report in the provider probe. Project sanitizer
coverage is **not** instrumented SDK coverage.

### Starting-main negative control

One minimal probe, `/tmp/gm9/control/negative_control_probe.cpp`, was used for
both revisions and never committed. It uses the private Fake backend, applies
and reads back `64x48@(8,4) 100 Hz 10 ms`, then requires a validated real SDK
frame.

Each revision was built in its own tree with its **own** private headers and
static archives. The unchanged starting main used a detached disposable
worktree `/tmp/gm9/wt-base` at `ae8de93`, with 0 changes. The feature used the
working tree. Neither probe loads any `gige_mel` shared object. `ldd` resolves
only the SDK DSO.

| | Starting main `ae8de93` | Feature |
| --- | --- | --- |
| `libgige_mel_aravis.a` members | `aravis_backend.cpp.o camera_profile.cpp.o` | `+ aravis_capture.cpp.o` |
| archive SHA-256 (prefix) | `a860ae18b60dcbd8` | `36a85bbe0ed3595a` |
| `aravis_private.hpp` SHA-256 (prefix) | `8924734fbf358b0b` | `35e7a17b329966aa` |
| `AravisStream::receive` symbol in probe | 0 | 1 |
| loaded SDK (`ldd`) | `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0` | same |
| result | **exit 1** after a successful configuration: `create_stream: code 4 (unsupported): Aravis image acquisition is not implemented …` | **exit 0**: `PASS validated real SDK frame 64x48 id 65401` |

The feature expectation therefore fails on starting main for exactly the
reason under test: streaming is unsupported.

### Other local checks

- **Offline verifiers.** Both pass (97 MEL files, 2 JSON files).
- **Verifier tests.** 29/29.
- **Installation.**
  - `scripts/verify_package_isolation.py` reports "package isolation verified"
    on real installs from `aravis-dev`, `aravis-release` and `aravis-clang`.
  - The SDK-free and SDK-enabled installs have identical non-documentation
    file sets.
  - The separately configured installed consumer, with `PKG_CONFIG_PATH`
    unset, passes.
  - The only installed non-documentation files containing the string "aravis"
    are two binaries whose debug info records the build directory name
    `build/aravis-dev`. This matches the GIGE-MEL-008 install, and the verifier
    checks symbols, not paths.
- **SDK absence.** With `PATH=/usr/bin:/bin` and an empty `PKG_CONFIG_PATH`,
  the default configure, build and CTest pass (8/8), with no Aravis or
  pkg-config cache entries. `-DGIGE_MEL_BUILD_ARAVIS=ON` fails with "requires
  exactly Aravis 0.8.36 …".
- **Synthetic capture.** `P5 64 48`.
- **Diagnostic.** `--help` exits 0. Malformed `capture` arguments exit 2
  before device I/O.
- **Direct capture.** The shared routine passed against Fake in scenario J,
  with the PGM header, 512×512 payload length and every pixel checked
  independently. The real `capture` command was not run against a GigE camera,
  because none is available. A disposable copy of the diagnostic, with only the
  backend swapped to Fake, printed the example output recorded in the Aravis
  document.
- **Whitespace.** `git diff --check` is clean over the full feature range.

**Not established.** This change provides no physical camera, GVSP transport,
camera timing, PTP/UTC, disconnection recovery or performance evidence, no
successful Aravis MEL Image publication, and no MEL conformance.

> **Superseded current-status statements.** The GIGE-MEL-008 records below
> say that `create_stream()` returns `unsupported`, that the SDK-enabled CTest
> total is 10 or 11, and that the SDK-free total is 7 or 8. They are historical
> results for those revisions and are kept unchanged.

## GIGE-MEL-008-R4 Python 3.9 compatibility of the verifier tests (2026-11-01)

This is an ordinary corrective commit on the reviewed R3 head, commit
`ab0ad422d751726b2be0d99cd0853fcb3ff946d2`, tree
`4f9391f644ea647b003bdc6e89338a2b6eb71007`. The local branch, the remote
branch and the PR #8 head all matched that revision, and the worktree was
clean. Main was unchanged: `039de755644ad2b15dd5827878ae71c028218e8f`, tree
`01476757e59fec54f66de1f3fa0820f0429b7375`. Only
`tests/verify_package_isolation_tests.py` and this record changed.
`scripts/verify_package_isolation.py` is byte-identical to R3. The C++
sources and tests, public headers and factories, CMake (including
`find_package(Python3 3.9 …)`), the workflow, manifests, vendored files,
provisioning and tools are unchanged.

**Finding.** R3's fixture helper `assert_actual_and_lexical()` called
`os.path.realpath(link, strict=True)`. CPython 3.9.0–3.9.22 has no `strict`
parameter (`def realpath(filename):`). The keyword reached the 3.9 series
only in 3.9.23. CMake accepts any 3.9 interpreter, so on those versions every
case that uses the helper raised `TypeError`. The R3 verifier itself and its
resolver evidence are unaffected.

**Correction.** The helper now uses the `pathlib` equivalent, which has
accepted `strict` since Python 3.6. The fixture check is still strict and
still independent of the verifier's component walker:

```python
self.assertEqual(link.resolve(strict=True), actual.resolve(strict=True))
```

The lexical-target comparison and the distinct-target assertion are
unchanged, as are all fixture arrangements, expectations and the 29 cases.

**Before/after on a native affected interpreter.** The tests ran in a
disposable container, with the system interpreter and the repository left
untouched. The image was `docker.io/library/python:3.9.22-slim-bookworm`
(`sha256:aff2066ec8914f7383e115bbbcde4d24da428eac377b0d4bb73806de992d240f`).

- **Interpreter:** `/usr/local/bin/python3`, reporting
  `3.9.22 (main, May 21 2025, 23:30:35) [GCC 12.2.0]` (CPython). Its
  signatures are `os.path.realpath(filename)` and
  `pathlib.Path.resolve(self, strict=False)`. Its `posixpath.py` is Git blob
  `af2814bdb0580410c25a2920b6a8b86a64d45042`, the CPython v3.9.22 blob.
- **Inputs:** both test files ran from read-only mounts, as a non-root user,
  against the same R3 verifier blob `bb2f7ebd…`. The test module itself
  reported the paths it selected.
  - The reviewed test file was extracted from `ab0ad42`, and its blob
    `1c2c974f…` was checked against the commit.
  - The corrected test file is blob `d09fc9bc…`.
- **Reviewed file, case 10:** errors with
  `TypeError: realpath() got an unexpected keyword argument 'strict'` at the
  helper.
- **Reviewed file, full suite:** 29 run, 21 pass, and 8 error with that
  `TypeError` in cases 10, 11, 12, 12b, 12c, 13, 14b and 14c.
- **Corrected file:** case 10 passes, and the full suite passes 29/29.

### Other local checks

- **Current interpreter:** 29/29 on `/usr/bin/python3` (3.13.5).
- **CTest:** `package_isolation_verifier` passes in the existing builds,
  which use `/home/zboll/.local/bin/python3.13` (3.13.16):
  - `build/dev` (SDK-free) passes 8/8;
  - `build/aravis-dev` (Aravis 0.8.36) passes 11/11.

  No C++ was rebuilt, and no sanitizer suite, real-install run or earlier
  R1–R3 control was repeated, because only a test helper changed.
- **Integrity:** `vendor_upstream.py --verify` (97 files) and `verify_json.py`
  (2 files) pass.
- **Whitespace:** `git diff --check` is clean over the R4 range and the full
  PR range.

The final commit, its tree and its hosted CI results are recorded in the R4
section of the PR #8 description.

## GIGE-MEL-008-R3 filesystem-order symlink resolution (2026-11-01)

This is an ordinary corrective commit on the R2 head, commit
`a7334529f6dc7bc672d54e1d056a0c79e828f8d3`, tree
`d928455c043705137448ab4699d1e882e8f68615`. The local branch, the remote
branch and the PR #8 head all matched that revision, and the worktree was
clean. Main was unchanged: `039de755644ad2b15dd5827878ae71c028218e8f`, tree
`01476757e59fec54f66de1f3fa0820f0429b7375`. Only the Python verifier, its
test suite and these two documents changed. The C++ sources and tests,
public interfaces, CMake, the workflow, manifests and provisioning are
unchanged.

### Finding and correction

R2's `resolve_link()` computed each hop as
`normpath(current.parent / target)`. That collapses `..` **before** any
preceding directory symlink is expanded. With
`share/doc/…/alias -> <outside>/a/b/c/d`, the public link
`lib/libextra.so -> ../share/doc/…/alias/../../../../lib/libextra.so.1`
lexically names the clean `<prefix>/lib/libextra.so.1`. The kernel resolves it
to `<outside>/lib/libextra.so.1`. R2 validated the lexical file and treated the
pair as a same-directory version link, so it skipped the public link. It then
scanned only the clean sibling and reported success.

R3 resolves each link in filesystem order, inside the canonical prefix, with
a queue of unresolved components. Any expansion that leaves the prefix fails.
All 16 permitted expansions count, including directory-component links. The
same resolved inode is opened with `O_NOFOLLOW|O_NONBLOCK`, classified, read
and passed to `nm`. The version-link exemption is evaluated only on that
**actual** target, which must be enumerated and lie outside the documentation
tree. See `docs/aravis-integration.md` for the full contract.

### Before/after: permanent fixture suite (stub `ldd`/`nm`/`cmake`)

The baseline verifier is the unchanged reviewed blob
`6ceb07ce071c0cf049acacde0b1a0cd8c42c18c0`, extracted with
`git cat-file` to `/tmp/gm8/r3/baseline/scripts/`. Its `git hash-object`
matches the blob, and its SHA-256 is `47c5b620…2a66`. The R3 test file was
copied unchanged next to it (`cmp` identical), so fixtures, stubs and
expectations are identical in both runs.

| Case | a733452 verifier | R3 verifier |
| --- | --- | --- |
| 01–09 (the 17 R2 cases) | pass | pass |
| 10 clean outside header via `alias/../clean.hpp` | **exit 0, "package isolation verified"** (`3 non-documentation text files are Aravis-free`) | `FAIL [installed-links] include/gige_mel/optional.hpp: link share/doc/…/alias -> <outside>/a/b/c/d resolves outside the installation prefix` |
| 11 outside forbidden library hidden by a clean lexical `.so.1` | **exit 0, "package isolation verified"** (`2 installed binaries have no Aravis symbols`) | `FAIL [installed-links] lib/libextra.so: link share/doc/…/alias -> … resolves outside the installation prefix`; the clean sibling is still `nm`-scanned and passes |
| 12 in-prefix composition, forbidden actual header | pass, only because R2 validated the lexical file but then read the *unresolved* public path, which the kernel resolved | pass, validating and reading the same resolved file |
| 12b in-prefix composition, forbidden actual library | **exit 0** (alias skipped as a "version link") | `FAIL [installed-symbols] lib/libextra.so contains … arv_camera_new` |
| 12c clean supported compositions | pass, but scans 2 binaries instead of the 3 actually reached | pass, scans 3 |
| 13 outside excursion returning inside | **exit 0** | two `installed-links` "outside the installation prefix" failures |
| 14 directory-component cycle | fails, but as "outside … or unresolvable" | "link chain exceeds 16 symlink expansions" |
| 14b exactly 16 expansions | **wrongly rejected** (final chain of 16) | pass |
| 14c 17 expansions | the directory chain is not counted | both chains fail at the 17th expansion |
| 15 FIFO/non-directory/unstatable components | **uncaught `PermissionError` traceback** | four explicit failures |
| 16, 16b ordinary version links and forbidden regular file | pass | pass |

Baseline: 29 run, 9 failed. R3: 29 run, 0 failed.

### Before/after: real compiled ELF files and real `nm`

`/tmp/gm8/r3/elf-demo` is a fresh real install of `build/aravis-dev`. Two
shared objects were compiled with `gcc -shared` and the same soname:

- the clean `<prefix>/lib/libextra.so.1` (`nm -D`: `T extra_clean`);
- the outside `<outside>/lib/libextra.so.1` (`nm -D`: `T arv_camera_new`).

The misleading public alias uses the exact link text above. `normpath` gives
`<prefix>/lib/libextra.so.1`; `realpath -e` gives
`<outside>/lib/libextra.so.1`. The verifier used the real SDK-enabled
adapter, provider, `ldd`, `nm`, `cmake` and consumer, not stubs:

- **a733452:** exit 0, "package isolation verified" (`3 installed binaries
  have no Aravis symbols`).
- **R3:** exit 1 with `FAIL [installed-links] lib/libextra.so: link
  share/doc/ams-gra-gige-mel-provider/alias -> <outside>/a/b/c/d resolves
  outside the installation prefix`.
- **Control:** the byte-identical forbidden ELF installed as an ordinary
  `lib/libextra.so.1` in a fresh prefix is rejected by **both** verifiers
  with `FAIL [installed-symbols] lib/libextra.so.1 contains Aravis/GObject
  symbols: arv_camera_new`.

### Permanent regressions and counts

`tests/verify_package_isolation_tests.py` grows from **17 to 29** cases. The
17 existing cases and their assertions are unchanged. The `nm` stub is also
content-aware: a `FIXTURE-SYMBOL=` marker affects only the file that carries
it, and the stub records each scanned path. The new cases do not use the
global `FIXTURE_NM_SYMBOL`. Each case first checks its fixture facts with
`realpath` and `normpath`, independently of the verifier. It then requires
the specific failure and an exact failure count. The new cases are 10, 11,
12, 12b, 12c, 13, 14, 14b, 14c, 15, 16 and 16b. CTest entries are unchanged:
**8** SDK-free and **11** SDK-enabled.

### Local results (executed)

- **Python suite:** 29/29 pass. It ran as uid 1000, so the
  unstatable-directory branch of case 15 was exercised.
- **CTest:** the GCC dev, Release and Clang 19 dev SDK-free builds pass
  **8/8** each. The GCC dev, Release and Clang 19 dev Aravis builds pass
  **11/11** each.
  - The C++ inputs were unchanged, so the existing builds were reused. The
    incremental build only reran the pinned-dependency check.
  - `package_isolation_verifier` also passes in the existing ASan, TSan,
    Aravis-ASan and Aravis-TSan build trees.
  - The sanitizer C++ suites were not rerun, because no C++ changed.
- **Offline verifiers:** `vendor_upstream.py --verify` (97 files) and
  `verify_json.py` (2 files) pass.
- **Real packages:** each of the dev, Release and Clang Aravis builds was
  installed to a fresh `/tmp/gm8/r3/real/<build>/gige-mel-aravis-install`.
  Each was verified with its own consumer build directory. In each run:
  - the adapter loads `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`;
  - the provider has 6 dependencies and none from the Aravis/GLib stack;
  - the install has 157 paths, 97 clean text files and 2 clean binaries;
  - the consumer cache has 0 SDK-discovery entry names, and the consumer
    builds and runs; and
  - the run ends with "package isolation verified".

  The only symlinks are `lib/libgige_mel_provider.so` and `.so.0`. Both
  resolve to `lib/libgige_mel_provider.so.0.1.0` and are exempt as version
  links. A logging `nm` wrapper around real `/usr/bin/nm` shows that the
  scan covered exactly `libgige_mel_backend.a` and
  `libgige_mel_provider.so.0.1.0`.
- **Whitespace:** `git diff --check` is clean over the R3 range and the full
  PR range.

### Hosted CI and final revision

The final commit cannot record its own hash. The R3 section of the PR #8
description records the pushed commit, its tree and its hosted results for
the push and pull_request workflows.

### Remaining limitations

The new symlink arrangements are verifier fixtures and controlled
demonstrations; they are not findings about the real package. The verifier
assumes a quiescent install tree; it is not a concurrent-filesystem or
file-descriptor-relative security design. The fixture `ldd`/`nm`/`cmake`
stubs exercise the verifier only. The installed-consumer qualification comes
from the real-package runs above. No hardware, camera, acquisition, clock
mapping or full MEL qualification is claimed.

## GIGE-MEL-008-R2 review corrections (2026-11-01)

This is an ordinary corrective commit on top of the R1 head: commit
`e82b812c4ee7852a2462694967f00b3dc9ae150c`, tree
`7257279aa1d62177038e2f06059ebd77f5d9a47f`. The branch had not advanced past
it. Main was unchanged (`039de75`). The environment, SDK prefix and SDK
instrumentation limits are the same as for R1 (the SDK, GLib and libxml2 are
uninstrumented).

### Corrections

- **A. Fresh ROI maxima are enforced according to the write order.** R1's
  `plan_axis()` never read a ROI feature's maximum. R2 checks each **written**
  feature against its fresh maximum before any write, on both axes. The check
  is skipped only when an **earlier write of the same plan decreases the
  companion** — offset written first and moving toward 0 for the size, or size
  written first and shrinking for the offset — because a smaller companion is
  the only supported way a coupled maximum can rise. It is not skipped when the
  companion is unchanged, increases, or is written later. Unchanged features
  are not written or judged, so current-state readback and recovery are
  unaffected.
- **B. SDK increment absence is distinguished from invalid values.** Per
  `arvgcfloat.c`/`arvgcfloatnode.c` at the pinned commit
  `97ff5e330919d7a44b871df4eda669feeafdf342`, `arv_gc_float_get_inc()` returns
  `G_MINDOUBLE` when the increment is undefined, and failures arrive as a
  `GError`, which is still checked first. Only `== G_MINDOUBLE` now becomes
  `std::nullopt`. R1's `increment > DBL_MIN` turned 0, negative values and NaN
  into absence. Those values now reach `check_limits()` and are rejected with
  feature and value context.
- **C. Installed symlinks are checked under the link's public-path policy.**
  R1 skipped any link resolving inside the prefix, through a tautological
  parent comparison. R2 follows each non-documentation link hop by hop (at most
  16 hops, every hop inside the prefix). The final target must be an existing
  regular file, and its content is scanned under the **link's** path policy.
  The only exemption is a same-directory `libX.so[.N]` → `libX.so.N.M` version
  link to an ELF file; that target is itself a non-documentation regular file
  scanned with symbol checks. Broken links, cycles, directory links and
  outside targets fail explicitly (`installed-links`). *(Superseded by R3: R2's
  resolution was not complete. It normalized each hop lexically (`normpath`),
  collapsing `..` before expanding a preceding directory symlink, so it could
  validate and scan a clean in-prefix file while the public link actually
  reached a different file outside the prefix. Its hop count and containment
  also ignored directory-component links. The 17 R2 fixture cases did pass as
  recorded below.)*
- **D. Documentation.** The R1 claim that `check_current` runs in
  `capabilities_from` is marked superseded above; the implementation is
  unchanged.

### Before/after controls

The baseline is **unchanged R1 production code**: `git archive e82b812` in
`/tmp/gm8/r2/ctl-ab`. Every adapter source, `cmake/GigeMelAravis.cmake` and
`CMakeLists.txt` were byte-identical to e82b812. The build used
`GIGE_MEL_SANITIZERS=address,undefined`, and the adapter was linked from that
build's own static `libgige_mel_aravis.a`, which has no `roi_maximum` symbol.
`ldd` shows `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`, version 0.8.36.
Only test code and harnesses were new.

**A — snapshot profile.** A separate harness linked `camera_profile.cpp` from
each revision (e82b812 blob SHA-256 prefix `ddfa4f1f34b7d0e0`; corrected
`bc8883dcb852c065`):

| Case | Expected | e82b812 | Corrected |
| --- | --- | --- | --- |
| A1: Height max 512, request 640×640 @ (0,0) | reject | **accepted** `Width=640 Height=640 ExposureTimeAbs=1000 AcquisitionFrameRate=25` | `invalid_argument` "Height 640 is above the fresh native maximum 512; no earlier write of this request decreases OffsetY …" |
| A2: OffsetY max 4, request 640×512 @ (0,8) | reject | **accepted** `Width=640 OffsetY=8 ExposureTimeAbs=1000 AcquisitionFrameRate=25` | "OffsetY 8 is above the fresh native maximum 4 …" |
| A3: Height max 512, OffsetY rises 0 → 16 | reject (no exemption) | **accepted** `Width=640 OffsetY=16 Height=640 …` | rejected |

**A — real SDK (Fake device).** A `<Max>` was added through the SDK's GenICam
DOM, and the SDK readback was asserted first. Every case used a fresh device:

| Case | e82b812: writes attempted → result | Corrected |
| --- | --- | --- |
| A1 | `Width, Height` → SDK "Value '640' greater than allowed maximum '512'"; partial state 640×512 | zero attempts, 512×512 unchanged |
| A2 | `Width, OffsetY` → SDK "Value '8' greater than allowed maximum '4'"; Width left at 640 | zero attempts |
| A3 | `Width, OffsetY, Height` → SDK rejection after two writes | zero attempts |

**B — real SDK extraction.** An explicit `<Inc>` was added to
`ExposureTimeAbs`, and the raw `arv_gc_float_get_inc()` result was recorded
with no `GError`:

| `<Inc>` | Raw SDK value | e82b812 open | Corrected open |
| --- | --- | --- | --- |
| *(none)* | `G_MINDOUBLE` (2.2250738585072014e-308) | succeeds | succeeds (absence); 1250.5 µs still quantizes to 1250 µs |
| `0` | 0 | **succeeds**; 1250.5 µs applied as if there were no increment | `unsupported` "ExposureTimeAbs advertises invalid increment 0 us" |
| `-5` | −5 | **succeeds** | rejected |
| `nan` | NaN | **succeeds** | rejected |
| `inf` | +inf | rejected (inf > `DBL_MIN` was kept, so R1 already rejected it) | rejected |
| `100` | 100 | — | grid enforced: 1050 µs rejected, 1010 µs applied |

Against the baseline, the committed adapter tests fail at the new assertions:
- "fresh ROI maxima by write order" fails in the shared `expect_rejected`
  helper, on the A1 expectation;
- "known SDK ROI maxima …" fails with "expected no native write attempt, but
  attempted: Width, Height";
- "SDK float increment …" fails because the `<Inc>0` open was not rejected.

The R1 regressions (native validation, fresh constraints, late failure) still
pass on the baseline, so these controls isolate the R2 findings.

**C — verifier.** The exact e82b812 verifier (SHA-256 prefix
`54786074284f904a`, equal to the blob) was run under the new permanent suite.
10 of 17 cases pass and 7 fail. Each failing case is accepted with exit 0:
- 03, a public-header symlink to forbidden content under the excluded
  documentation tree;
- 04, a broken public-header link;
- 05c, a broken same-directory library link;
- 05d, a library-named link to a documentation ELF;
- 06, a directory link;
- 06b, a link cycle;
- 06c, a link to a target outside the prefix.

The corrected verifier passes all 17. These are fixture controls with stub
`ldd`/`nm`/`cmake`, not findings about the real package.

### Permanent regressions and counts

- **Adapter scenarios: 18 → 21.** The three new scenarios are:
  - "fresh ROI maxima by write order": A1, A2, a rising companion, a companion
    decreased only later, both coupled transitions with the preceding
    bound-relaxing write asserted, and current-above-maximum recovery;
  - "known SDK ROI maxima rejected before the first write";
  - "SDK float increment absence versus invalid values". This covers proven
    `G_MINDOUBLE` absence; 0, −5, NaN and inf rejected at open with
    native-object and reservation cleanup and a clean reopen; `<Inc>0` added
    to an open device and rejected at `apply()` with zero writes and the same
    exposure feature (no "changed from"); and a valid increment of 100
    enforced.
- **Verifier suite:** new `tests/verify_package_isolation_tests.py`, 17
  `unittest` cases, standard library only. It is registered as the CTest
  `package_isolation_verifier`, so CTest grows from **7 → 8** (SDK-free) and
  **10 → 11** (SDK-enabled). It needs no SDK, so it runs in all five CI jobs,
  in both their SDK-free and SDK-enabled CTest invocations.
- **Unchanged:** provider 74, backend 8, portability 12 + 2.

### Local results

| Configuration | CTest | Provider / backend / portability | Aravis adapter | MEL probe |
| --- | --- | --- | --- | --- |
| GCC dev, Release, ASan+UBSan, TSan — SDK-free | **8/8** each | 74 / 8 / 12 + 2 (dev) | — | — |
| Clang 19 dev (`/usr/bin/clang++-19`), SDK-free | **8/8** | pass | — | — |
| GCC dev, Release, ASan+UBSan, TSan + Aravis | **11/11** each | 74 / 8 / 12 + 2 (dev) | **21/21** | pass |
| Clang 19 dev + Aravis | **11/11** | pass | 21/21 | pass |

Other checks:

- **Sanitizers:** run with `detect_leaks=1:halt_on_error=1` (ASan) and
  `halt_on_error=1` (UBSan, TSan); no reports.
- **Offline verifiers:** both pass.
- **Diagnostic and capture:** `--help` exits 0; capture produced a 64×48 PGM.
- **Real package isolation:** the corrected verifier passes on the real dev,
  Release and Clang SDK-enabled installs, each under a prefix named
  `gige-mel-aravis-install`. The real installs' only symlinks are
  `lib/libgige_mel_provider.so` and `.so.0`; both are accepted as version
  links, and their regular ELF target is symbol-checked.
- **Installed consumer:** the default install's consumer passes, and the
  SDK-free and SDK-enabled installs have identical non-documentation file
  sets.
- **Whitespace:** `git diff --check` is clean on both the corrective range and
  the full PR range.
- **Concurrency:** no concurrency code changed, so no repeat campaign was run.

### Remaining limitations

This is software and Fake-device evidence only. The maxima and increments were
arranged through the SDK's own GenICam nodes; no physical camera produced
them. The verifier symlink cases are fixtures. Acquisition, clock mapping,
physical-camera qualification and a second real SDK remain future work. The
two deferred PR #3 notes remain separate.

## GIGE-MEL-008-R1 review corrections (2026-10-31)

This is an ordinary corrective commit on
`feature/gige-mel-008-aravis-control` (PR #8). The starting revision was the
reviewed head, which had not advanced: commit
`315a0ed559b6b245e1ddc2b7697f65a2a0439d22`, tree
`5687099e0524e1abfe8714f4a8ff2f69f9cb43fc`. Main was unchanged at
`039de755644ad2b15dd5827878ae71c028218e8f` (tree
`01476757e59fec54f66de1f3fa0820f0429b7375`). The environment and the
provisioned Aravis 0.8.36 prefix are the same as in the GIGE-MEL-008 record
below. The SDK, GLib and libxml2 remain **uninstrumented**; the sanitizers
cover only the project's adapter, tests and provider.

### Corrections

- **A. Native facts are validated before arithmetic.** `check_limits` and
  `check_current` run on every snapshot inside `capabilities_from` and
  `plan_writes`; no caller is trusted. *(Superseded by R2: this sentence
  overstated the implementation. `capabilities_from` runs
  `check_preconditions` and `check_limits`; only `plan_writes` additionally
  runs `check_current`. The code was not changed to match the old wording.)*
  They cover:
  - sensor extent;
  - ROI increments, minimum ≤ maximum, and supported grid origins;
  - scalar bounds and increments;
  - current ROI geometry.

  ROI comparisons are written so that they cannot overflow
  (`offset ≤ sensor − size` instead of `offset + size ≤ sensor`). A zero
  requested size is rejected even if a native minimum would permit it.
  Structural safety is kept separate from truthful readback: `configuration_from`
  is **not** required before `apply()`, and current values need not lie inside
  newly reported bounds.
- **B. Fresh constraints in prevalidation.** `plan_writes` takes every limit
  from the fresh snapshot. The open-time envelope, which backs the immutable
  published capabilities, is used only to detect a changed sensor extent, ROI
  increment or selected exposure feature. Such a change is rejected as
  `unsupported` ("reopen") before any write. The following are final before
  writes:
  - the fresh rate bounds;
  - the exposure minimum and float increments;
  - the exposure maximum, except when the period lengthens first.

  Coupled ROI maxima stay with the SDK range check, which remains enabled.
  *(Superseded by R2: R1 never used the fresh ROI maxima at all. R2 enforces
  them before writes unless an earlier write of the plan decreases the
  companion.)* See `docs/aravis-integration.md` for the full policy.
- **C. Optional-feature query errors.** `present()` returns `Result<bool>`:
  - absent or successfully unimplemented → `false`;
  - implemented → `true`;
  - `GError` → a translated error with feature and operation context.

  All callers were audited:
  - optional strings;
  - the optional Boolean `AcquisitionFrameRateEnable`, now read through its
    feature node;
  - exposure selection, which no longer falls back after a failed query;
  - `inspect` identity strings, which report a failure as a problem.

  A node of the wrong type is an explicit diagnostic.
- **D. Package isolation is enforced.** The reviewed step used negated
  pipelines, which bash exempts from `errexit`. **The earlier green isolation
  steps, including the GIGE-MEL-008 hosted runs, therefore did not enforce the
  negative checks and are not isolation evidence.** The step now runs
  `scripts/verify_package_isolation.py`. Violations and check-execution
  failures are both fatal, and the consumer build cannot mask them. The
  filename scan examines only descendants of the prefix.
- **E1. Concurrent open.** The reviewed test released the first opener right
  after submitting the competitor, so it never demonstrated contention. The
  test now waits, with a bound, for a private hook to report that the
  competitor's real `try_lock` of the interface lock **failed** while the
  first opener was still gated inside `open()`. Only then does it release the
  first opener. `open()` now calls `try_lock` and then `lock()` on the same
  mutex; the hook is unset in production.
- **E2. Tolerance wording.** Comments and docs now state the formula
  `|v − n| ≤ 4·DBL_EPSILON·max(1, |n|)` and call it a relative epsilon
  tolerance. The numerical policy is unchanged. The "4-ULP" wording in the
  historical record below is left as written.

### Before/after controls (A, B, C)

All controls used the same provisioned SDK,
`/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`, confirmed both by `ldd` and
by the path the test binary prints. The setup was:

- The **reviewed production code** was extracted with `git archive 315a0ed`
  into `/tmp/gm8/r1/ctl-abc`.
- Every production file (`src/backends/aravis/*`, `cmake/GigeMelAravis.cmake`,
  `CMakeLists.txt`) was compared byte for byte with 315a0ed and was identical.
- The build used `GIGE_MEL_SANITIZERS=address,undefined`. The adapter is
  linked from that build's own static `libgige_mel_aravis.a`.
- Only `tests/aravis_adapter_tests.cpp` was replaced, by the corrected tests.
  Its concurrency scenario was compiled out, because the reviewed `TestHooks`
  has no lock hook; E1 has its own control below.

Run with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`:

| Finding | Scenario | Reviewed production | Corrected |
| --- | --- | --- | --- |
| A | INT64_MIN `Width` minimum (snapshot) | UBSan halt: `camera_profile.cpp:70:42: signed integer overflow: 640 - -9223372036854775808` | `unsupported`: "Width advertises minimum -9223372036854775808 …" |
| A | INT64_MAX current `Width`, offset 16 (separate process) | UBSan halt: `camera_profile.cpp:245:26: signed integer overflow: 16 + 9223372036854775807` | `unsupported`: "Width reads back 9223372036854775807 …" |
| A | INT64_MIN `Width` minimum imposed through the SDK | attempted OffsetX, Width, OffsetY, Height, ExposureTimeAbs, AcquisitionFrameRate | rejected, zero attempts, configuration unchanged |
| B | Fresh exposure max 500 µs, current 400 µs, rate 25 Hz, request 1 ms + new ROI | attempted OffsetX, Width, OffsetY, Height, ExposureTimeAbs before the SDK rejected the exposure | `invalid_argument` "ExposureTimeAbs 1000 us is outside the native bounds [10, 500]", zero attempts |
| B | Fresh `Width` grid 8 + k·8 (current 512), opened as 1 + k·1, request 641 | accepted on the old grid; attempted Width, ExposureTimeAbs, AcquisitionFrameRate | `unsupported` "Width increment changed from 1 at open to 8 … reopen", zero attempts |
| B | Late-failure retry after the reduced maximum is visible | attempted ExposureTimeAbs again | zero attempts (collector cleared before the retry) |
| C | TriggerMode = On with a failing implementation query (SDK GError "Port not found") | open **succeeded**: the error became absence and the trigger check was skipped | open fails: "query implementation of optional feature 'TriggerMode': [TriggerMode] [GigeMelTestBrokenImplementedForTriggerMode] Port not found for node …"; no writes; native object retired; a later clean open succeeds |
| C | Failing `AcquisitionFrameRateEnable` query on an open device | `configuration()` succeeded | explicit error |
| C | Failing `ExposureTime` query | silently selected `ExposureTimeAbs` | explicit error, no fallback |

The two UBSan cases ran in a separate harness,
`/tmp/gm8/r1/ubsan_controls.cpp`, with one process per case. It was linked
against `camera_profile.cpp` compiled from either revision:

- reviewed: SHA-256 prefix `bb2d9312c4dc7443`, equal to the 315a0ed blob;
- corrected: prefix `e2d19fb020e380c5`.

The expected-failing controls are kept out of the passing suite.

### Before/after controls (E1, D)

**E1.** The reviewed production code was built with **one** additive
private-header line: a `TestHooks::interface_lock_contended` declaration that
the reviewed `open_native` never calls. `aravis_backend.cpp` was
byte-identical to 315a0ed. Against that build, the corrected concurrency test
failed at its bounded 5 s wait for observed contention ("FAIL concurrent open
across adapters: … wait_for(…, 5s, …)"), so it cannot pass without evidence at
the locking boundary. With the correction it passed 10/10 bounded repeats in
the development build, plus the sanitizer runs.

**D.** These controls exercised the committed verifier and the exact workflow
step bodies extracted from the reviewed (315a0ed) and corrected `ci.yml`. They
ran under `bash --noprofile --norc -eo pipefail`, as GitHub Actions does.
Violations were arranged with PATH stubs (`ldd`, a `cmake --install` wrapper)
or in temporary copies of the real install. They are **controls**, not
findings about the real package. The only change to the step text was mapping
the SDK-location string to the local prefix.

| Case | Reviewed step | Corrected step / verifier |
| --- | --- | --- |
| Clean install (prefix `…/gige-mel-aravis-install`) | exit 0 | exit 0 |
| Forbidden provider dependency (`libglib-2.0.so.0` added to the provider's `ldd`) | violation printed, **exit 0** | exit 1: `provider-deps … depends on forbidden SDK libraries` |
| Installed text reference (`#include <arv.h> // aravis` in `backend.hpp`) | violation printed, **exit 0** | exit 1: `installed-text include/gige_mel/backend.hpp:226 references 'arv.h'` |
| Installed child filename (`include/gige_mel/aravis_backend.hpp`) | violation printed, **exit 0** | exit 1: `installed-names … mentions Aravis` |
| Package metadata `find_dependency(Aravis)` (verifier) | — | exit 1: `installed-text … GigeMelProviderConfig.cmake:32` |
| Child filename under a prefix inside an `aravis-root/` directory | — | exit 1, for the child only |
| `ldd` fails (stub exits 1) | — | exit 1: both `ldd` checks report the command failure |
| Unreadable installed header (`chmod 000`) | — | exit 1: `cannot read include/gige_mel/provider.hpp` |
| Untraversable installed directory | — | exit 1: `cannot traverse …/include/mel` |

In every corrected failing case the consumer step still ran, and it still
succeeded wherever the package allowed it; the overall exit status stayed
nonzero.

### Local results after correction

These results come from the final corrective tree, run in the existing preset
build trees with the same provisioned SDK:

| Configuration | CTest | Provider | Backend | Portability | Aravis adapter | MEL probe | Diagnostic `--help` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GCC dev, SDK-free | **7/7** | 74/74 | 8/8 | 12 + 2 | — | — | — |
| GCC Release, SDK-free | **7/7** | pass | pass | pass | — | — | — |
| ASan/UBSan, SDK-free | **7/7** | pass | pass | pass | — | — | — |
| TSan, SDK-free | **7/7** | pass | pass | pass | — | — | — |
| Clang 19 dev, SDK-free (`clang++-19`) | **7/7** | pass | pass | pass | — | — | — |
| GCC dev + Aravis | **10/10** | 74/74 | 8/8 | 12 + 2 | **18/18** | pass | pass |
| GCC Release + Aravis | **10/10** | pass | pass | pass | 18/18 | pass | pass |
| ASan/UBSan + Aravis (`detect_leaks=1:halt_on_error=1`) | **10/10** | pass | pass | pass | 18/18 | pass | pass |
| TSan + Aravis (`halt_on_error=1`) | **10/10** | pass | pass | pass | 18/18 | pass | pass |
| Clang 19 dev + Aravis (`clang++-19`) | **10/10** | pass | pass | pass | 18/18 | pass | pass |

The adapter suite grows from 10 to **18** scenarios. All 10 earlier scenarios
are retained; the changed ones are the profile fixtures, the concurrency test
and the late-failure retry. The 8 new scenarios are:

- native fact validation;
- fresh profile constraints;
- the fresh SDK exposure limit;
- the fresh SDK `Width` grid;
- unsafe SDK ROI facts;
- optional string, optional Boolean, and exposure-selection query errors.

`ldd` shows `libasan.so.8` + `libubsan.so.1`, and `libtsan.so.2`, in the
respective adapter binaries, plus the provisioned `libaravis-0.8.so.0`. No
sanitizer report occurred. The changed concurrency scenario passed 10 bounded
repeats in the development build and 5 each under ASan and TSan.

Other checks:

- **Offline verifiers:** both pass (97 MEL files; 2 JSON files).
- **Synthetic capture:** a 64×48 PGM was captured.
- **Diagnostic:** `gige_mel_aravis_diagnostic --help` exits 0.
- **Whitespace:** `git diff --check` passes on the corrective range and on
  the full PR range from main.
- **Real package isolation:** `scripts/verify_package_isolation.py` passes on
  the real installs from the dev, Release and Clang SDK-enabled builds, each
  under a prefix named `gige-mel-aravis-install`. This covers all checks:
  adapter SDK provenance, provider dependencies, 157 installed paths,
  97 non-documentation text files, 2 installed binaries, and a consumer
  configured without `PKG_CONFIG_PATH`. These are the configurations CI
  checks.
- **Sanitizer-build installs:** the five package checks also pass, but the
  verifier correctly fails the consumer step. An uninstrumented consumer
  cannot load an ASan/TSan-instrumented provider ("ASan runtime does not come
  first" / TSan deadly signal). This is expected, and is why CI runs the step
  only in the non-sanitizer jobs.
- **Install file sets:** the default install and the separately configured
  consumer pass. The SDK-free and SDK-enabled installs have identical
  non-documentation file sets.
- **SDK absence:** with `PATH=/usr/bin:/bin` and an empty `PKG_CONFIG_PATH`,
  the default configuration succeeds with no SDK lookups in its cache.
  `-DGIGE_MEL_BUILD_ARAVIS=ON` fails with "requires exactly Aravis 0.8.36",
  and a `.pc` file declaring 0.8.33 fails with "found aravis-0.8 0.8.33, which
  is not qualified". The build logic was not changed.

### Remaining limitations

Everything here is software evidence against the real Aravis 0.8.36 library
and its in-process Fake device:

- The malformed native values in A are either native-snapshot/profile
  regressions or values imposed through the SDK's own property nodes. No
  physical camera produced them.
- The query failures in C are genuine SDK `GError`s from GenICam nodes added
  through Aravis's public DOM API.
- The SDK is not sanitizer-instrumented.

Image acquisition, clock mapping, physical-camera qualification and a second
real SDK remain future work. The two deferred PR #3 notes remain separate.

## GIGE-MEL-008 optional Aravis camera-control foundation (2026-10-30)

Branch `feature/gige-mel-008-aravis-control` was created from freshly fetched
`origin/main`, which had not advanced since task preparation:

- Commit `039de755644ad2b15dd5827878ae71c028218e8f`
- Tree `01476757e59fec54f66de1f3fa0820f0429b7375`

**Environment.** Linux 6.12.113+deb13-amd64, GCC 14.2.0, Clang 19.1.7, CMake
3.31.6, Ninja 1.12.1, Python 3.13.5. Local provisioning used meson 1.5.2 in a
virtualenv and Debian 13 GLib 2.84 / libxml2 development packages unpacked into
a user-space sysroot (no root access). No camera or network discovery was used.

**SDK identity.** Aravis **0.8.36**:

- Archive `aravis-0.8.36.tar.xz` from the GitHub release. SHA-256
  `246deaa0042a387ff1bc00332d0fb80537ce14abde2c28d1a54c91f17adc51bf` was
  verified, and tag `0.8.36` resolves to commit
  `97ff5e330919d7a44b871df4eda669feeafdf342`.
- Built by `scripts/provision_aravis.sh` into the temporary prefix
  `/tmp/gm8/aravis-prefix`: release build, viewer, GStreamer, USB3, packet
  socket, introspection, docs and tests disabled, library target only. The
  SDK was **not** sanitizer-instrumented.
- Every test binary prints the library actually loaded:
  `/tmp/gm8/aravis-prefix/lib/libaravis-0.8.so.0`, runtime 0.8.36 and compiled
  0.8.36. `ldd` confirms the same path. The tests fail if the runtime version is
  not 0.8.36.

| Configuration | CTest | Provider | Backend | Portability | Aravis adapter | MEL probe | Diagnostic `--help` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GCC dev, SDK-free (`build/dev`) | **7/7** | 74/74 | 8/8 | 12 + 2 | — | — | — |
| GCC Release, SDK-free (`build/release`) | **7/7** | pass | pass | pass | — | — | — |
| GCC dev + Aravis (`build/aravis-dev`) | **10/10** | 74/74 | 8/8 | 12 + 2 | 10/10 | pass | pass |
| GCC Release + Aravis | **10/10** | pass | pass | pass | 10/10 | pass | pass |
| Clang 19 Debug + Aravis (`CXX=clang++-19`) | **10/10** | pass | pass | pass | 10/10 | pass | pass |
| ASan/UBSan + Aravis (`detect_leaks=1:halt_on_error=1`) | **10/10** | pass | pass | pass | 10/10 | pass | pass |
| TSan + Aravis (`halt_on_error=1`) | **10/10** | pass | pass | pass | 10/10 | pass | pass |

The SDK-enabled build adds exactly three CTest entries: `aravis_adapter`,
`aravis_mel_embedding` and `aravis_diagnostic_help`. The seven default entries
and their counts are unchanged. `ldd` showed `libasan.so.8` plus
`libubsan.so.1`, and `libtsan.so.2`, in the respective adapter test binaries.
The sanitizers instrumented the project's adapter, tests and provider only, not
Aravis, GLib or libxml2. No sanitizer startup or environment limitation
occurred.

Other checks:

- **Offline verifiers.** Both pass (97 MEL files; 2 JSON files). Neither
  manifest was changed.
- **SDK-free build.** The default build has no Aravis, pkg-config or GLib
  entries in `CMakeCache.txt`/`build.ninja`, and `ldd` of the provider and tests
  shows no Aravis or GLib. Synthetic capture works (64×48 PGM).
- **Install and consumer.** The default install and the separate installed
  consumer pass. Installing from the SDK-enabled build produces the identical
  non-documentation file set. No Aravis target, header, library or symbol is
  installed; only documentation mentions Aravis. A consumer configured with
  `PKG_CONFIG_PATH` unset builds and runs against that installed tree.
- **Whitespace.** `git diff --check` passes over the PR range.

### Controls

1. **SDK absence.** With `PATH=/usr/bin:/bin` and `PKG_CONFIG_PATH` empty, the
   default configuration succeeds. Its cache contains only the OFF option and
   no pkg-config/Aravis lookups. `-DGIGE_MEL_BUILD_ARAVIS=ON` fails at configure
   time with: "requires exactly Aravis 0.8.36; aravis-0.8 (or one of its
   GLib/libxml2 dependencies) was not found by pkg-config. Provision it
   explicitly …". A `.pc` file declaring 0.8.33 fails with "found aravis-0.8
   0.8.33, which is not qualified".
2. **Readback honesty.** Two mutations were made in a disposable copy
   (`/tmp/gm8/ctl`) against the same provisioned SDK:
   - **A.** `frame_rate_from_native` was made to round fractions, removing the
     4-ULP check. `aravis_adapter` failed in "SDK quantization and truthful
     readback" at `!thirty && … unsupported`, because 30.000300003 Hz became
     30 Hz. `aravis_mel_embedding` also failed, because the unrepresentable
     30 Hz command was accepted.
   - **B.** `apply()` returned the cached request instead of the readback. The
     same scenario failed at `quantized.value().exposure == 1'250'000ns`, since
     1 250 500 ns was falsely reported.

   With the implementation restored, `src/` in the copy is identical to the
   branch and the scenario passes.

No starting-main feature failure is claimed, because main has no optional
Aravis target. No broad mutation or repetition campaign was run.

### Recorded pinned-SDK behavior (Fake_1)

- Rates of 25, 40, 50, 1 and 1000 Hz read back exactly.
- 30 Hz gives period 33333 µs and reads back as `30.000300003000032` Hz. A
  period of 142857 µs reads back as `7.0000070000069998` Hz.
- `ExposureTimeAbs` 1250.5 µs reads back as 1250 µs.
- Range checking rejects `Width` 5000 with "greater than allowed maximum
  '2048'".
- Aravis's own `open_device` returns NULL with no GError for an unknown ID.
- The discovery strings change pointer on each refresh.

### Limitations

This is a camera-control foundation exercised against the real SDK and its
in-process Fake device. It is not an image backend: `create_stream()` is
unsupported. It is not a supported Aravis MEL profile, and it is not
qualification of GigE Vision, GenICam compatibility or any physical camera,
firmware, network or timing. The late-write failure is an SDK constraint
change arranged through `arv_gc_float_impose_max` from a private hook, not a
camera fault. Hosted CI results for the final head are recorded on the PR. The
two deferred PR #3 notes remain separate and untouched.

## GIGE-MEL-007 backend portability (2026-10-30)

Branch `test/gige-mel-007-backend-portability` was created from freshly fetched
`origin/main`, which had not advanced:

- Commit `14326c899eadd91417d3f48c0654d604eac41a51`
- Tree `cf9f5d8635e09497f7aa607429e6fcb9a6f23933`

Environment: Linux 6.12.113+deb13-amd64, GCC 14.2.0, Clang 19.1.7
(`clang++-19`), CMake 3.31.6, Ninja 1.12.1 and Python 3.13.5. No camera,
network or SDK was used.

**Production is unchanged.** The PR does not touch `src/`, `include/`, the 99
pinned dependency files, the factory signatures or the five-job workflow. The
new evidence qualifies the existing implementation. Added:

- `tests/support/contract_fixture_camera.{hpp,cpp}`: the independent test-only
  backend;
- `tests/provider_backend_contract_tests.cpp`: the shared scenarios;
- one CTest registration inside `BUILD_TESTING`;
- documentation in [backend portability](backend-portability.md).

| Configuration | CTest | Existing provider | Existing backend | New portability suite |
| --- | --- | --- | --- | --- |
| GCC dev | **7/7** | 74/74 | 8/8 | 12 shared runs (2 cases × 6) + 2 fixture checks |
| GCC Release | **7/7** | 74/74 | 8/8 | 12 + 2 |
| ASan/UBSan (`detect_leaks=1:halt_on_error=1`; `halt_on_error=1:print_stacktrace=1`) | **7/7** | 74/74 | 8/8 | 12 + 2 |
| TSan (`halt_on_error=1`) | **7/7** | 74/74 | 8/8 | 12 + 2 |
| Clang 19.1.7 Debug (`CXX=clang++-19`, separate `build/clang-dev`) | **7/7** | 74/74 | 8/8 | 12 + 2 |

CTest now has **seven** entries. The six previous ones are unchanged, and the
new one is `provider_backend_portability`. The test `pthread_join` forwarding
target resolved to `libasan.so.8` and `libtsan.so.2` respectively, so both
sanitizer runtimes were active. There were no sanitizer initialization
failures and no reduced settings.

Other checks:

- **Release build.** The first Release build of the new test failed because
  GCC 14 at `-O3` reported a `-Wfree-nonheap-object` false positive on a
  span-to-vector temporary. The test helper was rewritten (explicit copy plus
  an in-place `std::equal`); warnings were not relaxed.
- **Offline verifiers.** Both pass (97 MEL files and 2 JSON files).
- **Capture, install and consumer.** The synthetic capture works. The dev
  install and the separate installed consumer pass. Fixture references are
  limited to installed documentation; no fixture source, header, or symbol is
  installed.
- **Whitespace.** `git diff --check origin/main` passes over the whole change.
- **Bounded repetition.** Five dev runs each of the drain, fixture-direct and
  Health/BIT scenarios passed (15/15). Five TSan runs each of the drain and
  fixture-direct scenarios passed (10/10). Repetition supplements the
  deterministic join/gate/watchdog synchronization; it does not replace it.

### Starting-main and negative controls

- **Starting main.** An isolated detached worktree at `14326c8` was built with
  the dev preset; its own CTest passed 6/6. The provider DSO SHA-256 is
  `d0fec4bc…0b56`. The new test and fixture sources were compiled with the
  starting-main headers and linked to that DSO. `ldd` and `LD_DEBUG=files`
  (`calling init:`) confirmed `/tmp/gm7/base/build/dev/libgige_mel_provider.so.0`
  was loaded. Result: rc 0, 12 shared runs and 2 fixture checks passed. The
  existing provider needed no correction.
- **Mutation controls.** Each control is a disposable copy of starting main,
  one line changed in `src/provider.cpp`, with only the provider rebuilt. The
  same unchanged test binary was run against each mutated DSO using
  `LD_LIBRARY_PATH` (RUNPATH binary), and `calling init:` confirmed the loaded
  library each time.

| Control | Mutation | `[synthetic]` | `[contract-fixture]` |
| --- | --- | --- | --- |
| A (`3091dac6…`) | Row-by-row repack replaced by one `memcpy(destination, source, width*height)` | rc 0, 6/6 pass (tight rows hide it) | **rc 1**: `image publication and row repacking: pixel mismatch at x=0 y=1 frame=4294967280: got 165, model 127` (0xA5 padding leaked) |
| B (`d33b2e97…`) | Detailed `backend_id` reported as literal `"synthetic"` | rc 0, 6/6 pass | **rc 1**: `identity, instance lookup and discovery: values.at("gige_mel.backend_id") == kase.backend_id` |

The mutated copies were discarded, and final validation used the unmodified
provider. No broader mutation campaign was run.

### Limitations

Both cases are software backends. This record does not qualify a GigE Vision
SDK, physical camera, transport, packet behavior, disconnection, trigger
timing, clock mapping, PTP/UTC or calibration. It does not complete the
roadmap's "first real camera" or "second implementation" milestones. Hosted CI
results for the final head are recorded on the PR, not here. The two deferred
PR #3 notes remain separate and untouched.

## GIGE-MEL-003 configuration and command status (2026-10-28)

Starting freshly fetched main: commit
`d9d4a626ead7a0956702273ca292143164dc2ed5`, tree
`34b5546c5d225a64898de27d8653a1bee44df8d9`.
Linux x86-64, GCC 14.2.0, Clang 19.1.7, CMake 3.31.6 and Python 3.13.5;
all camera coverage uses synthetic sources or test-only decorators.

- GCC development, GCC Release and Clang Debug: CTest **6/6 passed** each.
- ASan/UBSan: **6/6 passed**, with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- TSan: **6/6 passed**, with `TSAN_OPTIONS=halt_on_error=1`.
- Native capture, install/export and separate installed-consumer build/run passed
  against GCC Debug, GCC Release and Clang Debug provider builds. Installed
  consumers exercise ConfigSet and status with no JSON headers installed.
- The provider suite retains all 29 previous groups and adds five configuration/
  status groups: schema/size/encoding and zero-apply rejection, shared transaction
  and ordering, every backend reason mapping and readback failure/mismatch,
  same/cross-provider reentry/throw/drain, and suppression/lifetime races.
  Existing fault-recovery coverage now also checks ConfigSet rejection/status.
- The complete development provider suite passed **100 consecutive repetitions**.
- Public-API negative control: `tests/configuration_probe.cpp` was compiled against
  an isolated starting-main library. With `--expect-unsupported` it confirmed valid
  ConfigSet returns Unsupported (with/without callback) and status registration
  returns NotSupported. Without the flag, the corrected feature expectation fails
  with exit 4. Against the feature, the CTest probe passes with correlated Accepted
  and both registered/unregistered ConfigSet success. No baseline runtime edits.
- Both offline dependency verifiers passed: all original 97 MEL files unchanged,
  plus two nlohmann/json files pinned separately. Parser source bytes were checked
  against the immutable upstream Git tree's blob identities before locking hashes.
- Final diff whitespace check passes; the immutable MIT license's original
  first-line trailing space is retained with a file-specific whitespace attribute.
- Workflow success criteria and the five hosted jobs are unchanged. Final-head
  hosted results are recorded in the PR/handoff, not inferred from local passes.

The exact schema, result/reason tables and callback/detach limitations are in
[configuration](configuration.md). No hardware, network transport, SDK, UTC/PTP,
automatic recovery or broader MEL conformance is claimed.

## GIGE-MEL-001 callback correction (2026-10-28)

The callback correction was validated locally on Linux x86-64 with GCC 14.2.0,
Clang 19.1.7, CMake 3.31.6, Ninja, and Python 3.13.16. This supplements the
historical bootstrap record below; all camera tests still use synthetic sources.

- GCC development, GCC Release, and Clang development builds: CTest **4/4 passed**
  in each configuration, including native loading and upstream integrity.
- ASan/UBSan: CTest **4/4 passed**, with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- TSan: CTest **4/4 passed**, with `TSAN_OPTIONS=halt_on_error=1`.
- Capture, installation, and separate installed-consumer build/run passed for
  GCC development, GCC Release, and Clang development provider builds.
- The provider suite now has **24 test groups**. Its three new groups cover the
  gated A-to-B nested communications callback versus external A detach,
  same/cross-provider mutation rejection in image and metadata callbacks, and
  nested metadata exceptions restoring callback context. Existing buffer lease,
  external drain, callback exception, and last-Control-reference tests pass.
- The development provider suite passed **100 consecutive repetitions**.
- Negative control: the new tests were copied into a disposable worktree at
  `81d5023eb9b7dde964bf56b666037fc338b26012` without the runtime correction.
  The first test established admission closure through detached keep-alive,
  released the nested callback gate, and failed with CTest's **30-second timeout**.
  With the correction the entire provider suite completes in about 0.33 seconds.
- `python3 scripts/vendor_upstream.py --verify` verified all 97 pinned files;
  `git diff --check` passed. No workflow, pinned header, public ABI, or backend
  interface changes were needed.

The five existing hosted jobs remain unchanged. Their results for the delivered
PR head are recorded in the PR/handoff, not inferred from these local runs.

## Executed checks

The bootstrap was compiled and exercised on Linux x86-64 with GCC 13.3.0,
CMake 3.31.6, Ninja, and Python 3.12. The code requires C++20; project warnings
were treated as errors. All checks below used the synthetic backend or an
explicit test decorator, not camera hardware.

| Check | Result | What it establishes |
| --- | --- | --- |
| Debug configuration and build | Passed | Provider, backend, example, and all test executables compile together against the pinned headers |
| Development CTest | **4/4 passed** | Backend contracts, provider contracts, standalone native loader, and upstream integrity |
| AddressSanitizer + UndefinedBehaviorSanitizer CTest | **4/4 passed** | The exercised paths completed without detected address or undefined-behavior errors |
| LeakSanitizer | **Enabled and passed** in the successful ASan run | No detected leaks in the exercised process lifetimes |
| ThreadSanitizer CTest | **4/4 passed** | No detected data races in the exercised concurrent paths |
| Standalone capture executable | Passed | Dynamically loaded provider delivered a 64×48 Mono8 frame and the client wrote binary PGM |
| Install/export package | Passed | Headers, libraries, notices, documentation, and CMake targets install successfully |
| Separate installed consumer | Passed | An independent `find_package(GigeMelProvider 0.1 CONFIG REQUIRED)` project compiles, links, initializes, and discovers the provider |
| Upstream byte integrity | Passed | All 97 locked upstream files match their provenance/checksum manifest |
| Project whitespace check | Passed | Newly authored tracked content has no Git whitespace errors; unchanged upstream files are excluded from style editing |

The four CTest entries are suite executables/checks, not four total assertions:
`backend_contract` contains **8 test groups**, `provider_contract` contains
**21 test groups**, and `native_loader` exercises factory resolution and complete
image delivery/teardown through the actual shared library.

The successful sanitizer runs used normal process inspection. A preliminary
LeakSanitizer attempt inside the restricted execution sandbox could not inspect
`/proc` and did not complete; the subsequent run with process inspection enabled
completed with **`detect_leaks=1`**. No leak check was disabled in the successful
validation result above.

## Backend coverage

The eight backend groups cover:

1. Stable device discovery, unknown-device rejection, and exclusive open.
2. Complete configuration validation, actual readback, and rejection while running.
3. Mono8 frame metadata, deterministic pixels, and retained-frame stability.
4. Bounded pool exhaustion, release recovery, and stream/configuration generations.
5. A blocked receive waking when a frame lease is released.
6. A blocked receive waking when the stream is stopped.
7. Device-close cancellation and frame bytes surviving complete backend teardown.
8. Move-only lease transfer and ownership release.

## Provider coverage

The 21 provider groups cover:

1. Factory validation, idempotent initialization, discovery, and channel identity.
2. Null/undersized/duplicate/overlapping/foreign registrations, flags, and mutation of bound buffers.
3. Retained MEL slots, starvation, explicit release, and delivery recovery.
4. Disable waiting for an active callback and preventing further image publication.
5. Delivered bytes and explicit release surviving channel detach.
6. Complete camera snapshots, response correlation, rejected inputs, and disabled-only configuration.
7. Geometry growth requiring sufficiently large registered memory.
8. Supported metadata registration, duplicate/empty registration, reentry, and C2-to-Image response routing.
9. Image callback mutation rejection without deadlock.
10. Unsupported command and callback results using actual MEL enums.
11. A throwing Image callback stopping acquisition while preserving its explicit buffer lease.
12. Control destruction detaching externally retained channels.
13. Destruction of an old detached channel preserving the replacement stream's active state.
14. Dropping the last external Control reference during an Image callback while another thread disables.
15. Detach draining an admitted metadata callback and closing subsequent admission.
16. A test backend producing padded rows and the provider publishing tightly packed rows.
17. Rejection of incomplete frames, stale configuration generations, and incompatible clock domains.
18. An invalid first frame not poisoning the accepted stream generation.
19. Clock generation changes and backward timestamps stopping publication.
20. A partially applied camera command reporting an error while retaining and using actual device readback.
21. Failed configuration readback blocking acquisition even with sufficiently large buffers.

The padded/faulting adapter is a test decorator around the synthetic backend.
It tests the abstraction boundary; it is not a second GigE SDK implementation.

## Native ABI and package coverage

`loader_tests` includes only upstream headers and links only system thread and
loader support. It resolves all three upstream factories at runtime, rejects
bad instance/manager combinations, registers caller-owned memory, validates
three frames, explicitly releases each delivered slot, and destroys the complete
provider object graph before unloading the DSO.

`tests/install_consumer` is configured as a separate project against an installed
prefix. It verifies exported target dependencies, public include paths, shared
library loading, the standard factories, initialization, and discovery without
using source-tree include paths.

## Reproducing the main checks

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev

cmake --preset asan
cmake --build --preset asan --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan
```

Use the install and capture commands in [README](../README.md) for those checks.
Sanitizer initialization depends on host process/memory-mapping capabilities;
record an initialization failure separately from a detected program defect.

## What this validation does not establish

No physical camera, Aravis, eBUS, pylon, or GenTL producer was used. Hardware
discovery, SDK compatibility, real network transport, bandwidth, packet resend,
physical unplug/reconnect, trigger timing, PTP/UTC mapping, calibration, and
mission-specific MEL behavior remain unqualified.

Clang and Release builds are configured in GitHub Actions but were not executed
locally in this bootstrap session. The GitHub workflow itself has not run because
no accessible remote repository was available. Configured CI is not reported as
a completed remote run. Tests and sanitizers cover the recorded scenarios; they
are not proof of complete MEL conformance or absence of all possible defects.
