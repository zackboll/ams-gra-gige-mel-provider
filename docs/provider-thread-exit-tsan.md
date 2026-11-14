# Provider thread-exit ownership investigation (GIGE-MEL-029)

## Disposition

Starting HEAD: `e9fdb62343e550c15085d3e035edadef9487ebfe`, canonical PR #26.
The reported deallocation/refcount race is an **external instrumentation
boundary**, combined with an **overstated test cleanup guarantee**. No
project-owned use-after-free or libstdc++ atomic reference-counting defect was
established. This is not inferred merely from a standard-library reproducer:
the actual self-detaching provider path was independently reproduced, audited
and compared with instrumented runtime thread-exit handling.

Production `src/provider.cpp` and all backend sources, including Pleora, are
unchanged. No MEL capability, timestamp policy, ABI or ownership policy changes.
The GenApi/log4cpp inversion is an independent, unresolved merge blocker; see
[its separate investigation](genapi-tsan-lock-order.md).

## Exact environment and evidence

Debian GCC `14.2.0-19`, C++20, Debug / `-O0 -g -fsanitize=thread
-fno-omit-frame-pointer -pthread`, system dynamically linked libstdc++/libtsan.
These are the original GIGE-MEL-028 runtime bytes:

| Runtime | SHA-256 | Build ID |
| --- | --- | --- |
| libstdc++.so.6 | `972bb2a18b71140dab0240f8a1f68ab3fb1d56bcd4c4f824a91b70888faf5a00` | `133b71e0013695cc7832680a74edb51008c4fc4c` |
| libtsan.so.2 | `d4d8d0b7b10a25c6a129e8049cff0228fa53d16d7f6f86560ad9d215402530d4` | `99ef88596cb10a8ccf307fe1a9070f66a44b1624` |

[Complete diagnostic stacks](provider-thread-exit-stacks.md) preserve the
original provider failure and the two matching independent control-block
reports, without truncating inlined allocator frames. Raw command logs,
disassembly, exit codes and all passing/failed invocations remain outside Git at
`/home/zboll/git/ams-gra-gige-mel-provider/build/029-evidence`.
Thirty original focused invocations passed; that does not invalidate the
original complete-suite failure.

## Happens-before and lifetime analysis

1. `HealthChannelImpl::enable()` publishes a new `Publisher` and starts a lambda
   owning `state`, a weak channel and `runtime`. Its temporary `control_pin`
   persists through enable's return. A new test gate prevents the callback from
   dropping external owners until that temporary owner is gone. Otherwise the
   test can accidentally exercise caller-thread teardown rather than self-detach.
2. `run()` locks Control and Health into scoped strong owners before `publish()`.
   Callback dispatch uses local callback copies, counts admission under the
   callback mutex, and runs consumer code outside runtime operations/state locks.
   Resetting `f.control` and `health` inside the callback does not destroy the
   source or its registered callable: the worker pins remain through callback
   return and dispatch cleanup. The tests check keepalive and live weak owners
   after the resets.
3. At the end of dispatch, `channel` is released before `control_pin`. Control
   still owns its channels. Releasing the last Control pin destroys Control on
   this worker, under the operations lock, detaches its channels and calls
   `stop_auxiliary_locked()`. That sets `running=false` under the Publisher mutex,
   notifies and selects the self-detach branch instead of joining itself.
   Dispatch admission has already drained. Health destruction repeats the stop
   safely with a nonjoinable thread handle.
4. No channel member is used after that destruction. `run()` uses the lambda's
   independently owned Publisher and Runtime to finish the stopped wait/loop;
   these captures, including the weak channel control block, survive until the
   thread callable is destroyed. Fixture C2/Image references remain live until
   the explicit exit barrier. This is the provider-specific lifetime audit,
   independent of the standard-library reproducer.
5. In GCC 14's `future.cc`, `_Make_ready::_S_run` promotes a retained weak state,
   release-stores ready, **then releases its temporary strong state and its
   notifier's weak state**. The main future's acquire observation orders work
   before the ready store, not the later notifier cleanup. The runtime does
   real atomic reference-count operations; they safely coordinate competing
   owners but their acquire/release edges are invisible in the uninstrumented
   library. The instrumented allocator/deleter can therefore report a conflict
   with the main thread's instrumented refcount decrement. System disassembly
   at `+0xdc591` matches the weak-control-block destruction return site and
   contains locked `xadd` decrements on the path.
6. Consequently, waiting for thread-exit readiness was not equivalent to joining
   the native worker and did not prove all shared-state cleanup complete.
   Replacing it with ordinary callback completion would be strictly weaker.
   Ordinary completion is explicitly demonstrated while exit is still blocked.

The detached/no-handshake library experiment also reports promise result-pointer
races: its ready store and the preceding writes occur across the same missing
runtime instrumentation boundary. A separate ordinary promise handshake after
`set_value_at_thread_exit()` removes that ambiguity and still reproduces the
**exact** original allocator/refcount report. No actual refcount implementation
bug is demonstrated by those reports.

## Independent experiments

Standalone sources: `tests/repro/promise_thread_exit`. They do not build as part
of normal CTest or install. All loops use fixed iteration counts, never
retry-until-pass. Invalid modes/counts return 2.

```sh
cmake -S /home/zboll/git/ams-gra-gige-mel-provider/tests/repro/promise_thread_exit \
  -B /home/zboll/git/ams-gra-gige-mel-provider/build/029-repro-tsan -G Ninja
cmake --build /home/zboll/git/ams-gra-gige-mel-provider/build/029-repro-tsan
TSAN_OPTIONS=halt_on_error=1 \
  /home/zboll/git/ams-gra-gige-mel-provider/build/029-repro-tsan/promise_thread_exit exit-detach 5000
```

| Experiment | Stock runtime TSan result |
| --- | --- |
| `value-join`, ordinary promise then native join | 5,000 passed |
| `exit-join`, deferred promise then native join | 5,000 passed |
| `value-detach`, ordinary promise, detached | 5,000 passed (not a general exit guarantee) |
| `exit-detach`, deferred promise, detached | Failed, result-pointer reports; diagnostic collection with `halt_on_error=0:exitcode=66` recorded four distinct warnings, exit 66 |
| `exit-detach-handshake`, separate promise orders deferred setter completion | 20,000 completed, exit 66, exact original deallocation/refcount report |
| `callback-vs-exit`, body signal while worker blocked, exit future not ready, then release and join | 5,000 passed |
| `provider_owners`, actual production self-detach, final Control/Health owners dropped in Subsystem callback, synthetic-only | 20,000 completed, exit 66, exact original report |

All six library modes passed 5,000 iterations each unsanitized; the actual
provider reproduction passed 20,000 unsanitized. No camera acquisition, Aravis
or Pleora is involved in these reproductions.

To build the actual-provider experiment, install the SDK-free TSan build and
configure the standalone project with `-DREPRO_PROVIDER=ON` and an absolute
`CMAKE_PREFIX_PATH` to that installation, then run `provider_owners 20000`.

### Runtime instrumentation comparison (experiment only)

Retrieved the unmodified matching GCC release sources:

- [future.cc](https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-14.2.0/libstdc%2B%2B-v3/src/c%2B%2B11/future.cc), SHA-256
  `e55b5945a4bf2024e22affc8d7d37ea63bd62e5cbb95ac8ed03534cefc642cf3`.
- [condition_variable.cc](https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-14.2.0/libstdc%2B%2B-v3/src/c%2B%2B11/condition_variable.cc), SHA-256
  `8d7776f730b290f71cebd34d32c5caed32e2d19284d98375962c52ce7b2e7015`.

Compiled both translation units with `g++ -std=c++20 -g -O0 -fsanitize=thread
-fno-omit-frame-pointer -pthread`. Linked into the library-only experiment:
5,000 `exit-detach`, 5,000 `exit-join` and 20,000 `exit-detach-handshake` passed. Also compiled them together with
`-fPIC -shared` into a temporary `instrumented-future.so` and ran the **unchanged**
actual provider reproduction with:

```sh
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtsan.so.2:/home/zboll/git/ams-gra-gige-mel-provider/build/029-evidence/instrumented-future.so \
TSAN_OPTIONS=halt_on_error=1 \
  /home/zboll/git/ams-gra-gige-mel-provider/build/029-repro-tsan/provider_owners 20000
```

All 20,000 passed. `LD_DEBUG=bindings` separately confirmed `_Make_ready::_M_set`,
`_S_run` and `__at_thread_exit` resolve to the instrumented implementation. This
is controlled instrumentation evidence, **not** a supported runtime replacement,
suppression, sanitizer exclusion or qualification-suite configuration. The first
attempt to link only `future.cc` failed with undefined `std::__at_thread_exit`;
including its matching implementation resolved it. No downloaded runtime source
or binary is committed. No fully instrumented libstdc++ build was performed;
this targeted comparison does not qualify every standard-library path.

## Small test-only correction and its limits

`test_join::SelfDetach` extends the existing Linux test-executable interposition
pattern. The callback arms a thread-local observation. Only that worker's
`pthread_detach(pthread_self())` is captured; the request is acknowledged to
`std::thread::detach()` while the native join right is handed to the test reaper.
No channel/Control owner is retained. All other detaches forward through
`RTLD_NEXT`, and joins still forward through the sanitizer interceptor, never
directly to libc. Missing self-detach admission fails closed via a watchdog;
CTest also bounds the test. Unwind opens callback gates before reaping, and the
reaper is destroyed before any captured fixture or promise.

Both Subsystem and BIT final-owner tests retain deferred readiness checks and
now additionally join before expired-owner/closed-channel assertions and promise
destruction. Subsystem explicitly proves ordinary callback-body completion does
not satisfy the exit future. The enable gate guarantees the self-detach branch
is actually selected, rather than racing enable's temporary Control pin.

**Limit:** this test reaper changes the selected native detach into a joinable
thread, not production ownership or teardown. Thus it cannot alone qualify the
real detached runtime cleanup. The separate noninterposed `provider_owners`
experiment above preserves and exercises that exact path, including its residual
stock-runtime TSan diagnostic. This limitation is deliberate and disclosed.

## Qualification

No sleeps, suppressions, exclusions, detector disablement or retry-until-pass
were introduced. Standard runs use `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`,
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Multi-warning diagnostic
experiments explicitly use `halt_on_error=0:exitcode=66` and retain failure status.
Existing Aravis SDK-boundary instrumentation policy is unchanged.

| Executed check | Result |
| --- | --- |
| Focused Subsystem/BIT final owner TSan, 200 invocations each | 400/400 passed |
| Explicit TSan reentry / teardown filters | 8/8 and 3/3 provider cases passed |
| Complete SDK-free Debug | 13/13 passed, including 100 provider cases and teardown/reentry regressions |
| Complete SDK-free ASan/UBSan | 13/13 passed |
| Complete SDK-free TSan | 13/13 passed |
| Complete Aravis/Pleora TSan, serialized | 24/25 passed; **FAIL** `pleora_sdk_contract`, known GenApi/log4cpp inversion, no additional finding |
| Complete Aravis/Pleora ASan/UBSan, serialized | 25/25 passed |
| Default install and separate consumer | Passed |
| Both-adapter Debug install, checked package isolation and separate consumer | Passed; 98 non-documentation text files, two installed binaries checked |
| Additional Pleora/GenApi installed dependency/interface/symbol check | 100 non-documentation files passed |
| Pinned upstream/JSON verification | 97/2 files passed |

Network fixtures were serialized across builds: combined ASan compilation was
paused before tests and resumed only after combined TSan completed. The initial
package verifier invocation with a relative install prefix failed consumer
configuration; the corrected absolute-prefix invocation passed. Both logs are
retained; the invocation error is not a product regression.

The historical provider failure, independent detached-runtime failures and
GenApi inversion remain recorded, not cancelled by passing reruns. Residual
qualification limitation: stock uninstrumented libstdc++ detached thread-exit
paths are not TSan-clean in this environment. No physical camera, SDK entitlement,
full MEL conformance or production readiness is qualified. PR #26 must remain
open, unmerged and without auto-merge. Publication commit/tree identities and
post-publication state are recorded in the PR report to avoid self-referential
commit hashes in this file.