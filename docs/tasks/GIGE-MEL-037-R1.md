# GIGE-MEL-037-R1 — Pleora register TSan qualification

## Reproduction and classification

Starting branch `feature/gige-mel-037-navigation-register-transport`, reviewed
HEAD `8410e1c7aad8e978108619d7c2f661de053c43e1`. No production defect was
demonstrated. The new process constructed a disconnected `PvDeviceGEV`, triggering
the previously reviewed external logging lifecycle inversion. It is a potential
deadlock report, not an observed hang or proof of general SDK concurrency safety.

Executed the requested command against the actual final-HEAD build:

```sh
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
ctest --test-dir build/036-both-tsan -R '^pleora_register_contract$' -V
```

CTest exited 8; the native process halted with sanitizer status 66. A separate
unsuppressed run used `halt_on_error=0:exitcode=66:second_deadlock_stack=1` and
`LD_DEBUG=libs`, so the complete contract and process cleanup could execute.
It completed, exited 66 and reported exactly one warning; no additional
sanitizer/runtime diagnostic. Full raw output and loader trace are retained
locally in `/tmp/037-r1-reproduce.log`, `/tmp/037-r1-unsuppressed-loaded.log` and
`/tmp/037-r1-runtime-identities.log`, not SDK artifacts in Git.

The warning was independently compared with the unchanged
`verify_unsuppressed` warning parser in `scripts/pleora_tsan_exception.py`:
only completion lines were substituted for that diagnostic-only comparison,
since the original executable did not run the eight GenApi groups. This was
investigation, not qualification of the old register CTest. Classification,
one report/summary, two-mutex order, all four vendor stacks, five offsets,
warning count and exit status matched exactly. Runtime SHA-256 and actual
initialization paths independently matched, not merely the library basename.

## Complete observed unsuppressed lock diagnostic

The original requested run reported the following (addresses/PID are incidental):

```text
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=2872210)
  Cycle in lock order graph: M0 (0x723000000598) => M1 (0x7f4e5a446a60) => M0

  Mutex M1 acquired here while holding mutex M0 in main thread:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1341 (libtsan.so.2+0x558ce) (BuildId: 99ef88596cb10a8ccf307fe1a9070f66a44b1624)
    #1 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1dba1)
    #2 __libc_start_call_main ../sysdeps/nptl/libc_start_call_main.h:58 (libc.so.6+0x29ca7) (BuildId: 5e5b51f4c39af9ac589408c1dc432ba390a273cc)

  Mutex M0 previously acquired by the same thread here:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1341 (libtsan.so.2+0x558ce) (BuildId: 99ef88596cb10a8ccf307fe1a9070f66a44b1624)
    #1 log4cpp_GenICam::Category::addAppender(log4cpp_GenICam::Appender&) <null> (liblog4cpp_gcc48_v3_4.so+0x1a75f)
    #2 __libc_start_call_main ../sysdeps/nptl/libc_start_call_main.h:58 (libc.so.6+0x29ca7) (BuildId: 5e5b51f4c39af9ac589408c1dc432ba390a273cc)

  Mutex M0 acquired here while holding mutex M1 in main thread:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1341 (libtsan.so.2+0x558ce) (BuildId: 99ef88596cb10a8ccf307fe1a9070f66a44b1624)
    #1 log4cpp_GenICam::Category::removeAllAppenders() <null> (liblog4cpp_gcc48_v3_4.so+0x1a47a)
    #2 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d609)

  Mutex M1 previously acquired by the same thread here:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1341 (libtsan.so.2+0x558ce) (BuildId: 99ef88596cb10a8ccf307fe1a9070f66a44b1624)
    #1 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d765)
    #2 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d609)

SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock) (/home/zboll/sdk/pleora/ibex-mirror/docs/place_in_opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64/lib/genicam/bin/Linux64_x64/liblog4cpp_gcc48_v3_4.so+0x1dba1)
```

The continuing unsuppressed run additionally printed the original register
completion and `ThreadSanitizer: reported 1 warnings`, then exited 66. The original
halt-on-error run necessarily stopped before completion. Both acquisition orders
occur on the main thread; none of the reported acquisition frames is project-owned.

## Actual loaded identities and isolation

Environment: Linux x86-64, GCC 14.2, Debug, TSan; Aravis 0.8.36,
eBUS 6.5.3.7155 and GenApi 3.4.1. Both hardware CTests OFF. External SDK root
`/home/zboll/sdk/pleora/ebus_sdk` resolves to
`/home/zboll/sdk/pleora/ibex-mirror/docs/place_in_opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64`.
The loader initialized each of the following from that root's
`lib/genicam/bin/Linux64_x64/` directory, exactly once. `readelf -n` shows no GNU
build ID for these four external files; hashes identify them:

| Library | SHA-256 |
| --- | --- |
| libGenApi_gcc48_v3_4.so | `9a2f6a5f2f6e85b22154ad467f75acafd1d1ec197122ea39e32d29df69ad0565` |
| libGCBase_gcc48_v3_4.so | `7534579ef0e4d513f569d97f26998c7378a322d2d58ea1d5d0d0ebced83604a4` |
| libLog_gcc48_v3_4.so | `935ded42d13cc143c73eb97a9b77f37ce4b4d6ae913370f148a33840b942a1a2` |
| liblog4cpp_gcc48_v3_4.so | `e053cc5f25f3ccea7aa573e6616565413cd1abc29e1b94fdd7f930830ce7544f` |

These match the [independent investigation](../genapi-tsan-lock-order.md), not
an updated allowlist. Temporary project-source copies, compiled/linked with the
same Ninja commands and native libraries, established the trigger:

* A marker at main entry and markers before/after `PvDeviceGEV` construction all
  printed before the warning. The loader trace places log4cpp initialization
  after transfer to main, not at initial process loading.
* Removing only the disconnected-device block retained the complete recording
  contract, actual PvResult calls and identical library link inputs. It exited 0,
  with no warning. Native device construction is required in this test to trigger
  the logging lifecycle; the warning is observed during subsequent native cleanup.

Isolation source/commands and logs remain under `/tmp/037-r1-isolate.py`,
`/tmp/037-r1-marked.cpp`, `/tmp/037-r1-marked.log`,
`/tmp/037-r1-no-device.cpp` and `/tmp/037-r1-no-device.log`.

## Confined correction: Option A

All four disconnected register/memory calls and their original assertions move
into a helper called by the first existing group of `pleora_sdk_contract`.
The helper uses a separate disconnected native device, as before; no discovery,
connect, stream or physical camera operation is introduced. All recording/native
status tests remain in `pleora_register_contract`, running unsuppressed normally.

The existing SDK audit now additionally requires exactly one native-register
completion marker before the first group's PASS, in both phases. All eight
original groups and final completion remain required. No warning matcher,
runtime fingerprint, suppression directive, CMake test scope or production code
changes. The two-phase SDK audit still requires the exact single inversion
unsuppressed, full completion and status 66 before running the same full native
contract with the reviewed function-specific exception. Every other diagnostic,
identity mismatch, crash, incomplete test, unexpected exit or timeout fails.
There is no retry-to-green path. SDK-free negative tests verify these properties,
including that baseline failure never reaches the scoped phase.

## Qualification and disposition

Exact corrected commit/tree and executed full nine-lane matrix, installed
consumers/isolation, integrity/publication and hosted CI belong on PR #31.
Configured checks are not presumed passes. No physical camera was used.

The correction addresses sanitizer test-process organization, not the vendor
deadlock or transport SDK guarantees. The
[execution boundary and development-only scope evaluation](../device-register-transport.md#development-only-merge-scope-versus-production-qualification)
remain explicit: one application SDK call is not one packet, no wire retry,
a hard completion bound or sensor acceptance. A guaranteed finite production
INavigationSink call bound is not established. Maintainer acceptance of deferral
for a development-only foundation is not assumed; until accepted the final
disposition is QUALIFICATION_GAP even if every applicable development test passes.
