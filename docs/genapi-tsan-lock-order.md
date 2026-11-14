# GIGE-MEL-028 — native GenApi lock-order investigation

## Disposition

**External dependency isolated; combined TSan qualification remains BLOCKED.**
No suppression is approved or installed. The original `pleora_sdk_contract`
remains enabled and failing under TSan. No production implementation, fixture,
test registration, sanitizer setting, factory, advertised capability or SDK
library was changed. Work is confined to canonical PR #26, starting at
`32ab174ca8e2042c65df28f5d0f6a1762fd2a40a`.

The same inversion occurs in a standalone program using only external GenApi,
GCBase and Log, with one empty-category XML document and an ordinary stack-local
`CNodeMapRef`. It needs no eBUS `Pv*` library, `Features`, profile, provider,
IPort, project fixture, camera, network, worker thread or feature read/write.
An earlier two-library build (GenApi/GCBase, before adding explicit logging
experiments) also reproduced it. GenApi loads log4cpp dynamically.

This is a **potential deadlock report**, not an observed hang. Both observed
orders occur on the main thread at different phases. That does not establish
that arbitrary embedding applications are safe: concurrent logging/reconfiguration
and shutdown have not been proven mutually exclusive throughout the SDK.
Neither this report nor successful non-TSan tests qualifies production use.

## Exact environment and library identity

Executed on Debian 13 x86-64, kernel `6.12.116+deb13-amd64`, GCC
`14.2.0-19`, CMake **3.31.6** / Ninja **1.12.1**, Debug. The unchanged existing combined build is
`/home/zboll/git/ams-gra-gige-mel-provider/build/027-tsan`, with:

- `GIGE_MEL_BUILD_PLEORA=ON`, `GIGE_MEL_BUILD_ARAVIS=ON`;
- `GIGE_MEL_SANITIZERS=thread`, warnings-as-errors ON;
- both physical hardware test options OFF;
- external eBUS root `/home/zboll/sdk/pleora/ebus_sdk`, resolving to
  `/home/zboll/sdk/pleora/ibex-mirror/docs/place_in_opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64`;
- matching external headers at
  `/home/zboll/sdk/gige-mel-027-genapi/headers/library/CPP/include`;
- eBUS **6.5.3.7155**, GenApi **3.4.1.1**, original `gcc48_v3_4` runtime;
- external Aravis **0.8.36** at `/home/zboll/sdk/aravis-0.8.36`;
- `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`.

CTest supplies `LD_LIBRARY_PATH=$SDK/lib:$SDK/lib/genicam/bin/Linux64_x64`,
`GENICAM_ROOT_V3_4=$SDK/lib/genicam`, and its build-local
`GENICAM_CACHE_V3_4`. No deadlock suppression or detector-disable option is used.
Existing separate Aravis **race** suppressions remain unchanged; they are not
linked into the Pleora contract or SDK-only reproduction. Thus “no suppression”
here means no suppression of this diagnostic, not removal of prior Aravis policy.

SHA-256 of the actual loaded external runtime files:

| File in `lib/genicam/bin/Linux64_x64` | SHA-256 |
| --- | --- |
| `libGenApi_gcc48_v3_4.so` | `9a2f6a5f2f6e85b22154ad467f75acafd1d1ec197122ea39e32d29df69ad0565` |
| `libGCBase_gcc48_v3_4.so` | `7534579ef0e4d513f569d97f26998c7378a322d2d58ea1d5d0d0ebced83604a4` |
| `libLog_gcc48_v3_4.so` | `935ded42d13cc143c73eb97a9b77f37ce4b4d6ae913370f148a33840b942a1a2` |
| `liblog4cpp_gcc48_v3_4.so` | `e053cc5f25f3ccea7aa573e6616565413cd1abc29e1b94fdd7f930830ce7544f` |

These four ELF files expose no GNU build ID in `readelf -n`; hashes identify
them instead. System `libtsan.so.2` SHA-256 is
`d4d8d0b7b10a25c6a129e8049cff0228fa53d16d7f6f86560ad9d215402530d4`,
build ID `99ef88596cb10a8ccf307fe1a9070f66a44b1624`.

## Original contract: complete TSan lock stacks

Command from `/home/zboll/git/ams-gra-gige-mel-provider`:

```sh
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  ctest --test-dir /home/zboll/git/ams-gra-gige-mel-provider/build/027-tsan \
  -R '^pleora_sdk_contract$' -V
```

Result: CTest exit **8**, test failed; child TSan exit **66**. Repetition also
failed. Below are all four captured acquisition stacks (common absolute project
prefix shortened to `$REPO`; the vendor's missing symbols remain `<null>`, not
invented symbol names):

```text
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)
Cycle: M0 (0x7230000001d8) => M1 (0x7f6665e46a60) => M0

Mutex M1 acquired while holding M0, main thread:
 #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1341
    (libtsan.so.2+0x558ce)
 #1 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1dba1)
 #2 pleora_node_map::Fixture::Fixture() $REPO/tests/support/pleora_node_map.cpp:143
    (pleora_contract_tests+0x31c41)
 #3 main $REPO/tests/pleora_contract_tests.cpp:318 (pleora_contract_tests+0x19636)

Mutex M0 previously acquired by same thread:
 #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1341
    (libtsan.so.2+0x558ce)
 #1 log4cpp_GenICam::Category::addAppender(log4cpp_GenICam::Appender&)
    (liblog4cpp_gcc48_v3_4.so+0x1a75f)
 #2 pleora_node_map::Fixture::Fixture() $REPO/tests/support/pleora_node_map.cpp:143
    (pleora_contract_tests+0x31c41)
 #3 main $REPO/tests/pleora_contract_tests.cpp:318 (pleora_contract_tests+0x19636)

Mutex M0 acquired while holding M1, main thread:
 #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1341
    (libtsan.so.2+0x558ce)
 #1 log4cpp_GenICam::Category::removeAllAppenders()
    (liblog4cpp_gcc48_v3_4.so+0x1a47a)
 #2 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d609)

Mutex M1 previously acquired by same thread:
 #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1341
    (libtsan.so.2+0x558ce)
 #1 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d765)
 #2 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d609)

SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock)
         (liblog4cpp_gcc48_v3_4.so+0x1dba1)
```

The original executable build ID is `304e70b058d1adea8a3ee6f54ce6c331b88979da`.
TSan cannot unwind all optimized/stripped vendor frames. Its `+0x1d609` frame
is not sufficient to identify a public function; debugger evidence below
establishes the actual phase independently.

## Lock ownership and conflicting paths

Read-only inspection of the external matching headers and actual ELF code shows:

- **M0:** the category's `Category::_appenderSetMutex`. Both `addAppender`
  and `removeAllAppenders` lock the mutex at category object offset `+0x58`
  (`pthread_mutex_lock` call sites `+0x1a75b`, `+0x1a476`).
- **M1:** the singleton hierarchy maintainer's `_categoryMutex` protecting the
  category map. The singleton base is library-relative `+0x246a20`; its mutex
  is at `+0x40`, hence `+0x246a60`, matching TSan's global mutex address.
  The category enumeration path locks it at `+0x1db9d`; the exit-cleanup
  path locks it at `+0x1d761` and then calls `removeAllAppenders`.
- During initial logging configuration, `addAppender` holds M0, invalidates
  enabled caches through `Category::InvalidateEnabledCache(bool)`, enumerates
  categories, and acquires M1. During global cleanup, the maintainer holds M1
  and removes category appenders, acquiring M0. **Both locks and both nested
  paths belong to the vendor's log4cpp runtime.** Neither is a provider operation
  lock, a project lifetime mutex, a node-map port lock, or an SDK buffer lock.

No proprietary implementation is copied here. Names come from public matching
headers/exported symbols; internal roles are corroborated by read-only call-site
and storage-offset inspection, not a claim to possess vendor debug symbols.

## Independent reproduction and phase isolation

The original source and independent CMake project are in
`/home/zboll/git/ams-gra-gige-mel-provider/tests/repro/genapi_lock_order`.
It is deliberately not included in the provider build, installed package or
SDK-free hosted CI; it does not turn an expected sanitizer failure into a pass.
Run with an existing authorized external SDK and matching headers:

```sh
REPO=/home/zboll/git/ams-gra-gige-mel-provider
SDK=/home/zboll/sdk/pleora/ebus_sdk
HEADERS=/home/zboll/sdk/gige-mel-027-genapi/headers/library/CPP/include
cmake -S "$REPO/tests/repro/genapi_lock_order" -B "$REPO/build/028-repro-tsan" -G Ninja \
  -DGENAPI_INCLUDE_DIR="$HEADERS" \
  -DGENAPI_LIBRARY_DIR="$SDK/lib/genicam/bin/Linux64_x64"
cmake --build "$REPO/build/028-repro-tsan"
env LD_LIBRARY_PATH="$SDK/lib/genicam/bin/Linux64_x64" \
  GENICAM_ROOT_V3_4="$SDK/lib/genicam" \
  GENICAM_CACHE_V3_4="$REPO/build/028-repro-tsan/genicam-cache" \
  TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  "$REPO/build/028-repro-tsan/genapi_lock_order" raii
```

| Mode | Experiment | Observed result |
| --- | --- | --- |
| `empty` | Construct/destroy empty reference; never load XML | Exit 0, no diagnostic; negative phase control, **not** qualification |
| `raii` (default) | Load minimal XML; destroy reference before returning | Exit 66, same four vendor lock sites; also 5/5 independent repeat processes failed |
| `destroy` | Explicit public `_Destroy()` before reference destructor | Exit 66, same inversion at global teardown |
| `initialize` | Public `CLog::GetLogger("")` before constructing/loading map | Exit 66, same inversion; moves initial edge earlier only |
| `shutdown` | Public `CLog::ShutDown()` after map/reference destruction | Exit 66 inside shutdown, same inversion; moves reverse edge earlier only |

In `raii`, the complete marker sequence precedes the warning:

```text
phase: entered main
phase: constructed empty reference
phase: XML loaded
phase: reference scope destroyed
phase: returning from main; global teardown follows
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)
```

The SDK-only stacks have exactly the same `+0x1dba1`, `+0x1a75f`,
`+0x1a47a`, `+0x1d765`, `+0x1d609` frames; the first two stacks end at
`main`, `genapi_lock_order.cpp:40`, instead of `Fixture::Fixture`/contract main.
The `shutdown` stacks additionally include `GenICam_3_4::CLog::ShutDown()`
at `libLog_gcc48_v3_4.so+0x32b2` on both reverse-edge stacks.

For deeper callers, build the same source with `-DREPRO_SANITIZERS=` in
`$REPO/build/028-repro-debug`, preserving the above loader environment, then:

```sh
gdb -q -batch -x "$REPO/tests/repro/genapi_lock_order/trace.gdb" \
  --args "$REPO/build/028-repro-debug/genapi_lock_order"
```

Debugger caller chains (top to bottom; vendor offsets retained for stripped
frames) establish initialization versus exit independently of TSan unwinding:

```text
Category::addAppender(Appender&)                    log4cpp +0x1a740
??                                                log4cpp +0x29cfb
??                                                log4cpp +0x2c663
PropertyConfigurator::configure(istream&)          log4cpp +0x28722
??                                                log4cpp +0x36a3a
??                                                log4cpp +0x37ffc
GenICam_3_4::CLog::DefaultInitializer()             Log     +0x2d47
GenICam_3_4::CLog::Exist(gcstring const&)           Log     +0x2eed
GenICam_3_4::CLog::Exist(char const*)               Log     +0x2f3e
??                                                GenApi  +0xf3387
CNodeMapFactory::CNodeMapFactoryImpl::CreateNodeMap GenApi  +0x1449f9
CNodeMapRefT::_LoadXMLFromString                   NodeMapRef.h:375
main                                              genapi_lock_order.cpp:40

[XML load returns, reference destroyed, main returns]

Category::removeAllAppenders()                     log4cpp +0x1a460
?? [hierarchy exit cleanup]                        log4cpp +0x1d77a
__run_exit_handlers                               exit.c:118
__GI_exit                                         exit.c:148
__libc_start_call_main                             libc_start_call_main.h:74
__libc_start_main_impl                             libc-start.c:360
_start
```

The same debugger breakpoints on the uninstrumented original native contract
confirm initialization at `Fixture::Fixture`'s first XML load and the direct
`+0x1d77a -> removeAllAppenders` reverse path in `__run_exit_handlers`, after
contract main returns. Additional `removeAllAppenders` calls occur during
initial PropertyConfigurator setup and later category destructors; their mere
presence is not the reported reverse edge. All eight native test groups execute
successfully without TSan. Project `Features` borrows the node map; the fixture's
port outlives its map by member declaration order. Neither lifetime explains the
independent reproduction.

## Supported ordering investigation and rejected mitigations

The matching external `GenApi/NodeMapRef.h` documents ordinary RAII and public
`_Destroy()`; `Log/CLog.h` documents default log4cpp initialization, public
`GetLogger`, and `ShutDown` as logging deinitialization. The supported surface
experiments above all retain the inversion. `GenApi/DLLLoad.h` also declares
`GenApiTerminate()` for releasing DLLManager resources, but the actual bundled
GenApi runtime exports no such symbol: a direct link experiment fails with
undefined reference. It is not an available shutdown fix for this runtime.

No SDK-supplied ordering requirement establishing an application obligation to
manually alter the hierarchy was found. No vendor support confirmation or
replacement runtime was available in this investigation. In particular, adding
process-global logger shutdown to a reusable provider would not be justified:
it could invalidate another SDK client and still reproduces the failure.
No direct internal hierarchy calls, logger replacement/omission, log-config
changes, library patches/replacements, intentional leaks, `_Exit`, or skipped
destruction were adopted. Those change semantics or hide diagnostics rather
than establish a supported ordering correction.

A `deadlock` suppression matching the exact demangled
`log4cpp_GenICam::Category::removeAllAppenders()` frame could technically be
narrower than a whole-library suppression. It would still suppress **any**
future cycle involving that frame, not just this pair of mutexes or this vendor
hash. TSan suppression matching cannot encode phase/lock-pair/library SHA.
Therefore it requires explicit owner approval, vendor disposition, version/hash
scope and a removal/retest condition. It is **not added or used**, and approving
one would be a separate qualification-policy decision, not evidence that this
suite passes unsuppressed. Preferred disposition: vendor-confirmed fix and a
separately reviewed compatible SDK update, followed by full requalification.

## Qualification evidence

Executed on the unchanged production/test sources plus this standalone
reproducer and documentation (GCC 14.2, unless indicated):

| Check | Result |
| --- | --- |
| Original native contract, repeated TSan with both stacks | **FAIL**, CTest 8 / child 66, same external inversion |
| First complete combined Aravis + Pleora TSan | **24/25 passed, 1 failed** (`pleora_sdk_contract`); CTest 8, gate BLOCKED |
| Post-commit combined TSan repeat at `17ea9961a4cdddb837436db4efb0caa51bf23744` | **23/25 passed, 2 failed** (`provider_contract`, `pleora_sdk_contract`); see additional blocker below |
| Complete combined Aravis + Pleora ASan/UBSan | **25/25 passed**, leak detection and halt-on-error enabled |
| Complete combined unsanitized Debug | **25/25 passed** |
| Pleora-only Debug | **18/18 passed** |
| Aravis-only Debug | **20/20 passed** |
| SDK-free default Debug | **13/13 passed** |
| SDK-free ASan/UBSan | **13/13 passed** |
| SDK-free TSan | **13/13 passed** |
| SDK-free GCC Release / Clang 19 Debug (hosted-equivalent local builds) | **13/13 passed each** |
| SDK-only reproducer, unsanitized / ASan+UBSan | All five modes exit 0; invalid mode exits 2 in unsanitized build |
| Upstream / JSON integrity | 97 / 2 pinned files verified |
| Combined install / package verifier / separate installed consumer | Passed |
| Additional Pleora isolation check | Common provider dependencies/symbols and installed C++/CMake interfaces remain vendor-free |
| Proposed files vs external SDK content | No exact file-content matches; scanner rules pass |

The first combined TSan run reported no additional races, invalid synchronization
operations or different lock-order cycles. **That observation did not hold on
the post-commit repeat:** it also reported a `provider_contract` data race. No
invalid synchronization or different lock-order cycle was reported. Existing
Aravis race-suppression policy is unchanged. Simulator tests were run sequentially
across builds to avoid UDP 3956 collisions.

### Additional qualification blocker found on post-commit repeat

**GIGE-MEL-029 follow-up:** [provider thread-exit investigation](provider-thread-exit-tsan.md)
isolates the matching detached shared-state report to the uninstrumented runtime
boundary and strengthens both final-owner tests with a native exit/cleanup
barrier. The historical diagnostic below is retained. The independent GenApi
inversion remains unresolved; stock detached-runtime TSan qualification remains
limited as described in the follow-up.

This is separate from the conclusively isolated GenApi inversion and must not
be hidden by a successful rerun. `provider_contract` reported:

```text
WARNING: ThreadSanitizer: data race
Write of size 8, Health worker T276:
 operator delete(void*, unsigned long)                    libtsan +0x9be2b
 std::__new_allocator<..._State_baseV2...>::deallocate     provider_tests +0x3ff156
 std::__allocated_ptr<...>::~__allocated_ptr              provider_tests +0x3e9cca
 std::_Sp_counted_ptr_inplace<...>::_M_destroy             provider_tests +0x409c0e
 <null>                                                  libstdc++.so.6 +0xdc591
Previous atomic write of size 4, main thread:
 __gnu_cxx::__exchange_and_add                            ext/atomicity.h:66
 __gnu_cxx::__exchange_and_add_dispatch                   ext/atomicity.h:101
 std::_Sp_counted_base<...>::_M_release                   shared_ptr_base.h:359
 std::__shared_count<...>::~__shared_count                provider_tests +0x3858d8
 std::__shared_ptr<...>::~__shared_ptr                    provider_tests +0x372257
 std::shared_ptr<...>::~shared_ptr                        provider_tests +0x372283
 std::promise<void>::~promise                            provider_tests +0x3726cb
 subsystem_owners                                       provider_subsystem_tests.inc:824
 main                                                   provider_tests.cpp:2862
Worker creation:
 pthread_create                                         libtsan +0x568a6
 std::thread::_M_start_thread                            libstdc++.so.6 +0xe12f8
 HealthChannelImpl::enable                              src/provider.cpp:1395
 subsystem_owners                                       provider_subsystem_tests.inc:819
 main                                                   provider_tests.cpp:2862
```

The test uses `promise::set_value_at_thread_exit()` and waits for the future
before destroying its promises. The system libstdc++ thread-exit callback is
not instrumented. Its SHA-256 is
`972bb2a18b71140dab0240f8a1f68ab3fb1d56bcd4c4f824a91b70888faf5a00`,
build ID `133b71e0013695cc7832680a74edb51008c4fc4c`.
A local standard-library-only experiment (no SDK/provider/test support) with a
detached `std::thread`, `set_value_at_thread_exit()`, timed future readiness wait,
and ordinary promise destruction also reports a race under this GCC/libstdc++
configuration. That suggests a separate standard-library instrumentation boundary;
it does **not** conclusively resolve the exact reported control-block race or
justify calling the provider failure a pass. Local experiment/logs are retained
under `build/028-evidence/promise_exit.cpp` and `promise-exit-timed-tsan.log`.
The independent experiment is:

```cpp
#include <future>
#include <thread>
int main() {
    for (int i = 0; i < 5000; ++i) {
        std::promise<void> done;
        auto exited = done.get_future();
        std::thread worker([&] { done.set_value_at_thread_exit(); });
        worker.detach();
        if (exited.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
            return 2;
    }
}
```

Compile with `g++ -std=c++20 -g -O0 -fsanitize=thread
-fno-omit-frame-pointer -pthread` and run with the same `TSAN_OPTIONS` above.
The immediate focused provider rerun passed all 100 cases. Repeated passing
runs do not invalidate the earlier diagnostic. No test/production change or
suppression was made on this evidence. Further isolation of this intermittent
report remains a qualification blocker; “no new races” cannot be confirmed.

Publication identity and final-HEAD checks are recorded in PR #26. Hosted CI runs
on the new commit are distinguished there from the executed local hosted-equivalent
builds. Raw local investigation logs stay in the ignored directory
`/home/zboll/git/ams-gra-gige-mel-provider/build/028-evidence`; no SDK bytes,
camera images, binaries, downloads or raw private evidence are committed.
The applicable combined TSan gate cannot pass while the original contract
reports this inversion. No physical camera or entitlement test is implied.

## GIGE-MEL-030 — subsequent scoped development exception

The investigation above records the original unsuppressed diagnosis and its
then-unresolved status. A subsequent test-only, pinned-runtime exception is
described in [the GIGE-MEL-030 contract](pleora-tsan-exception.md). The native
test must still reproduce the exact known cycle unsuppressed before the
function-specific rule may be applied. All other findings remain failures.
