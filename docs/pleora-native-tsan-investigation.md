# Pleora native TSan investigation — GIGE-MEL-040-R2

**Classification: `VENDOR_THREAD_LIFECYCLE_FAILURE` (primary), with a contributing
`SANITIZER_RUNTIME_DEFECT_OR_LIMITATION`.** No project harness or production
defect was found, and no project source was changed to avoid the failure.
**Native TSan remains BLOCKED and unwaived.** Production integration remains blocked
by separate licensing, receiver-policy, controller-coexistence and strict
additional-GVCP gates.

Design: [GIGE-MEL-040-R2 design](tasks/GIGE-MEL-040-R2-design.md). Measurements:
[GIGE-MEL-040-R2 validation](tasks/GIGE-MEL-040-R2-validation.md). Shareable vendor
summary (not sent): [Pleora escalation record](pleora-tsan-vendor-escalation.md).
Background: [stream-only audit](pleora-stream-only-audit.md).

## Question and answer

R1 recorded a fatal TSan CHECK while `PvPipeline::Stop()` was running:

```text
ThreadSanitizer: CHECK failed: sanitizer_thread_registry.cpp:348 "((t)) != (0)"
  ThreadRegistry::ConsumeThreadUserId <- pthread_detach (TSan interceptor)
  <- PtUtilsLib::Thread::WaitUntilSignaled() <- PvStreamLib::Pipeline::Stop()
```

R2 shows the CHECK does **not** require a pipeline. The minimal reproducer is
**S2: PvStreamGEV construct, `Open`, `Close`, destroy**. No pipeline, buffer,
receive, frames or PvDevice is involved. In S2 the CHECK occurs inside
`PvStream::Close()` →
`StreamGEV::Close()` → `DataReceiverUM2::Close()` → `DataReceiverUM2Thread::StopThread()`
→ `PtUtilsLib::Thread::WaitUntilSignaled()` → `pthread_detach`.

Debugger breakpoints on exported **glibc** functions independently observed this
sequence on one `pthread_t` (the stream data-receiver thread) in every traced run:

1. `pthread_create` → **0**, called from `PtUtilsLib::Thread::StartThread()` during
   `PvStreamGEV::Open()`; the attribute is joinable.
2. `pthread_join` → **0**, from `WaitUntilSignaled()` during `Close()`.
3. `pthread_detach` on the **same, already joined** `pthread_t`, also from
   `WaitUntilSignaled()`. Ordinary glibc returns **3 (ESRCH)**, and `Close()` then
   returns OK. Under TSan, the interceptor finds no live registry entry
   for that handle. Its CHECK aborts the process (exit 66) before the real detach runs.

POSIX makes detaching an already joined thread undefined, because the handle no
longer identifies a thread. The SDK-free invalid controls reproduce the identical
CHECK with this exact `create → join → detach` pattern. That pattern is the
primary defect. The process abort is TSan's handling of the defect, which is the
contributing runtime factor (see below).

## Evidence policy and limits

The eBUS EULA forbids reverse compilation and disassembly, so none was done. The
observations used are:

* the program's exit status and stage markers;
* TSan's own stacks;
* gdb breakpoints on glibc `pthread_create/join/detach/exit`, glibc `start_thread`
  and sanitizer `CheckFailed`;
* gdb's ordinary unwinder and dynamic symbol names;
* external strace (`clone3`, futex, exit).

No vendor breakpoints, memory writes, interposition, binary patching or license
changes were used. Breakpoints perturb timing; the ordinary and TSan matrix runs
were made **without** a tracer and agree with the traced runs.

**What is not claimed.** Why `WaitUntilSignaled()` joins and then detaches is not
known: it could be a timed-wait fallback or defensive cleanup. Whether other
eBUS versions behave the same is also not known. Only the observable sequence is
asserted.

## Scenario matrix

Each cell is a fresh process in a disposable two-namespace veth topology, with
both packet observers and verified cleanup. Results (`matrix1`, retained privately):

| Scenario | Operations | GCC Debug | Clang Debug | GCC ASan/UBSan | GCC TSan | Clang TSan |
| --- | --- | --- | --- | --- | --- | --- |
| S0 | SDK initialization, exit | 2/2 pass | 2/2 pass | 2/2 pass | 2/2 pass | 2/2 pass |
| S1 | PvStreamGEV construct/destroy | 4/4 pass | 4/4 pass | 4/4 pass | **report** exit 66 (4/4 stages) | **report** exit 66 (4/4 stages) |
| S2 | Open/Close, no pipeline | 7/7 pass | 7/7 pass | 7/7 pass | **CHECK** in Close, 4/7 | **CHECK** in Close, 4/7 |
| S3 | PvPipeline construct/destroy, no Start | 10/10 | 10/10 | 10/10 | **CHECK** in Close, 7/10 | **CHECK** in Close, 7/10 |
| S4 | Pipeline Start/Stop, no receive | 14/14 | 14/14 | 14/14 | **CHECK** in Stop, 7/14 | **CHECK** in Stop, 7/14 |
| S5 | Start, timed receive, Stop | 15/15 | 15/15 | 15/15 | **CHECK** in Stop, 8/15 | **CHECK** in Stop, 8/15 |
| S6 | Start, genuine buffer retrieve/release, Stop | 15/15 | 15/15 | 15/15 | **CHECK** in Stop, 8/15 | **CHECK** in Stop, 8/15 |
| S7 | Two Start/Stop lifecycles on one stream | 21/21 | 21/21 | 21/21 | **CHECK** in first Stop, 7/21 | **CHECK** in first Stop, 7/21 |
| S8 | Close after Stop and quiescence | 15/15 | 15/15 | 15/15 | **CHECK** in Stop, 7/15 | **CHECK** in Stop, 7/15 |
| D1 | Direct PvStream queue/timed retrieve/abort, idle | 11/11 | 11/11 | 11/11 | **CHECK** in Close, 7/11 | **CHECK** in Close, 7/11 |
| D2 | D1 with genuine frames | 11/11 | 11/11 | 11/11 | **CHECK** in Close, 7/11 | **CHECK** in Close, 7/11 |

Pass means exit 0, every stage marker in order, `SCENARIO_COMPLETE`, no sanitizer
diagnostic, and verified cleanup and observers. For the CHECK cells, the stage count
is the markers reached and the location is the call in progress (no `STOP_RETURNED`
marker in Stop scenarios). Every cell retired its children, namespaces, interfaces
and packet sockets. The host network inventory was unchanged.

S6 and D2 genuine buffers keep operation result **1538 NO_LICENSE** in every
variant; this is unchanged R1 entitlement evidence, not pixel delivery.

### Separate findings (not merged)

* **S1 — previously audited GenApi/log4cpp inversion.** This is a TSan *report*,
  not a CHECK. It is a `lock-order-inversion` between
  `log4cpp_GenICam::Category::addAppender` and `removeAllAppenders`.
  Its external offsets (`liblog4cpp_gcc48_v3_4.so+0x1dba1`, `+0x1a75f`,
  `+0x1a47a`, `+0x1d609`, `+0x1d765`) match the R1 record exactly. All stages
  complete; exit 66 comes from `exitcode=66`. It is **not** the registry CHECK, is
  not waived here, and the existing narrowly scoped GenApi exception
  (`deadlock:log4cpp_GenICam::Category::addAppender`, applied only to
  `pleora_sdk_contract` after an unsuppressed exact-report audit) is unchanged.
* **S2 and later — distinct fatal thread-registry CHECK** at
  `sanitizer_thread_registry.cpp:348`, under `WaitUntilSignaled()`, in Close or
  Stop. A CHECK is fatal and is not governed by `halt_on_error`, `exitcode` or
  suppressions.
* **Ordinary native runs (GCC and Clang Debug)** complete every scenario, and all
  checked SDK results are OK, including `Pipeline.Stop` and `Close`.
* **ASan/UBSan** produced no diagnostic in any scenario (leak detection on).
* **Native buffer operation** is `1538 NO_LICENSE` (S6, D2).

## Pthread lifecycle observation

gdb traces (`--tracer gdb`) record each glibc entry and return, with the
`pthread_t`, return value, LWP, monotonic timestamp and unwound stack. Under TSan,
the interceptor and the real glibc call are both observed; only the outermost call
is counted. A trace counts as evidence only if it has exactly one `trace_ready`,
`inferior_exit` (with a status) and `trace_finished`. Final-source S2 traces:

| Variant | pthread_t | create | join (Δ after create) | detach (Δ after join) | Exit |
| --- | --- | --- | --- | --- | --- |
| GCC Debug | `0x7fac6d7fe6c0` | 0 | 0 (+93.6 ms) | **3 ESRCH** (+465 µs) | 0 |
| Clang Debug | `0x7f890b7fe6c0` | 0 | 0 (+95.8 ms) | **3 ESRCH** (+514 µs) | 0 |
| GCC TSan | `0x7f49853fe6c0` | 0 | 0 (+112.8 ms) | never returned (libtsan interceptor, +395 µs) → CHECK :348 | 66 |
| Clang TSan | `0x7f10eddfe6c0` | 0 | 0 (+93.0 ms) | never returned (static runtime interceptor, +270 µs) → CHECK :348 | 66 |

In each row, create was called from `PtUtilsLib::Thread::StartThread()` under
`DataReceiverUM2Thread::StartThread` ← `DataReceiverUM2::Open` ←
`PvStreamGEV::Open`, with start routine `PtUtilsLib::WorkLink(void*)`.
Join and detach were both called from `PtUtilsLib::Thread::WaitUntilSignaled()` under
`DataReceiverUM2Thread::StopThread` ← `PvStream::Close()`. The traced thread's LWP
exited before the join returned, and no other create returned the same
`pthread_t`. Hypotheses ruled out:

* **H3** (invisible creation): every detached handle came from an observed create.
* **H5** (handle reuse): no other create returned the same `pthread_t`.
* **H4** (detached-then-exited): the thread was joined, not detached, beforehand.
* Double detach: there was one detach per handle.

Earlier S4 traces show the **same** sequence twice in an ordinary run:

* the pipeline worker created in `Pipeline::Start()`, joined and detached (ESRCH)
  in `Pipeline::Stop()`;
* then the stream receiver, during `Close()`.

Under TSan, the first such detach (in Stop) aborts.

glibc 2.41 explains the ordinary result. A successful join sets `pd->tid = -1`,
and `pthread_detach` returns ESRCH when `tid < 0`. The real detach is therefore
harmless only because the thread descriptor has not yet been reused. That is not
a guarantee once the handle is invalid.

## Sanitizer mechanism and independence

GCC 14.2 `libsanitizer` and LLVM 19.1.7 `compiler-rt`
`sanitizer_thread_registry.cpp` are **byte-identical** (SHA-256
`ba5138d2f45647abb2402c95bb6c7ae21e269164de414631f138cafc68f3069b`). Both
`pthread_detach` interceptors call `ThreadConsumeTid` → `ConsumeThreadUserId`
**before** `REAL(pthread_detach)`. Line 348 is `CHECK(t)` after
`live_.find(user_id)`. A successful intercepted join already removed the entry.
The GCC (dynamic `libtsan.so.2`) and Clang (static `libclang_rt.tsan`) runs are
therefore two builds of one registry implementation, **not two independent
confirmations**. Their agreement shows that the result does not depend on the
compiler or on how the runtime is linked.

**Contributing factor.** TSan aborts the process with an internal CHECK on a
stale handle. It does not issue a diagnosable report. That prevents any further
analysis of the run, and no option or suppression can be applied to it. This is
recorded as a sanitizer limitation; it does not make the SDK behaviour valid.
LLVM issue #156829 (AArch64, Clang 19.1.7, same `WaitUntilSignaled` frame) is
related reporting only, not proof.

## SDK-free controls

`tests/repro/pthread_tsan_controls` builds separate valid and negative
executables. It is standalone and never part of ordinary CTest.

| Control | glibc (GCC and Clang, uninstrumented) | GCC TSan | Clang TSan |
| --- | --- | --- | --- |
| create → join | 0, 0 | clean | clean |
| create → detach | 0, 0 | clean | clean |
| create → thread completes → join | 0, 0 | clean | clean |
| **Invalid:** detach → detach | 0, then 22 EINVAL | **CHECK :348** | **CHECK :348** |
| **Invalid:** join → detach | 0, then **3 ESRCH** | **CHECK :348** | **CHECK :348** |
| **Invalid:** detach → exit → detach | 0, then 22 EINVAL | **CHECK :348** | **CHECK :348** |

Valid lifecycles never produced the CHECK. The negative controls are
expected-failure characterizations: `negative_control_outcome()` accepts only the
exact CHECK with a nonzero exit. They never justify the invalid pattern.

## Harness audit (H1, project misuse)

S2 uses only documented calls on one stream and checks every result:

1. `PvStreamGEV()`.
2. `Open(PvString("10.64.40.2"), 41040, 0, PvString("10.64.40.1"), 16)`.
3. `Close()`.
4. Destruction.

It uses no pipeline, buffers, PvDevice or callbacks, and all calls are on the main
thread.

Pipeline scenarios follow the shipped `PvPipelineSample` teardown order:
`SetBufferCount`, `Start`, `RetrieveNextBuffer`/`ReleaseBuffer`, `Stop`, delete
the pipeline, `Close`. Every retrieved buffer is released before Stop.

Direct scenarios follow `PvStreamSample`: queue, `RetrieveBuffer` with timeout,
`AbortQueuedBuffers`, drain, then Close. **One recorded deviation:** the sample
frees its buffers *before* `Close()`, whereas D1/D2 free them *after* `Close()`.
This cannot explain the CHECK: S2 has no buffers and D1/D2 abort in Close before
any buffer is freed.

The samples obtain the stream through a PvDevice and `PvStream::Free`. The
harness constructs `PvStreamGEV` directly, without an application-side PvDevice,
which is the stream-only use under investigation.

The same invalid sequence occurs with **no** harness thread or buffer activity,
and the production adapter performs the same `Stop()`/`Close()` calls. **H1 is
rejected**; there is nothing in project code to fix. **H6** (a
runtime defect independent of the SDK) is rejected because valid lifecycles never
CHECK. **H7** is not supported: the ordinary and TSan traces contain the same
calls in the same order.

## Dispositions

* `READY_FOR_INDEPENDENT_REVIEW`: diagnostic tooling and root-cause evidence only.
* `NATIVE_TSAN_BLOCKED`: genuine Pleora Open/Close and `Pipeline.Stop` still fail
  TSan with the fatal CHECK. This is not suppressed, waived, skipped, converted or
  relabelled. Ordinary CTest and CI never run these native scenarios.
* `PRODUCTION_INTEGRATION_BLOCKED`: licensing (1538 NO_LICENSE), receiver policy,
  controller coexistence and strict zero-additional-GVCP (Gate C FAIL) remain
  unresolved, independently of TSan.

Resolution requires a vendor fix or a vendor-documented shutdown sequence that
avoids the join-then-detach. A project workaround (e.g. never calling Close) would
leak SDK threads and is not acceptable. A TSan option, suppression or exception
cannot apply to a CHECK and must not be attempted.

## Explicit reproduction (opt-in; never a physical camera)

Prerequisites are external and must already exist: eBUS 6.5.3.7155 SDK, GenApi
3.4.1 headers, pinned Aravis 0.8.36 (S6/D2 only), unprivileged user and network
namespaces, gdb (optional). Output must be a new private directory outside the
repository. Captures, logs and runtime paths are **not approved for publication**.

```sh
repo=/home/zboll/git/ams-gra-gige-mel-provider
cmake -S "$repo" -B "$repo/build/040r2-gcc-tsan" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ -DGIGE_MEL_SANITIZERS=thread \
  -DGIGE_MEL_BUILD_PLEORA=ON -DGIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT=ON \
  -DGIGE_MEL_PLEORA_SDK_ROOT=/home/zboll/sdk/pleora/ebus_sdk \
  -DGIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR=/home/zboll/sdk/gige-mel-027-genapi/headers/library/CPP/include
cmake --build "$repo/build/040r2-gcc-tsan" --target pleora_tsan_stage_audit --parallel 2
env -u TSAN_OPTIONS python3 "$repo/scripts/pleora_tsan_stage_audit.py" \
  --binary "$repo/build/040r2-gcc-tsan/pleora_tsan_stage_audit" \
  --sdk /home/zboll/sdk/pleora/ebus_sdk --output /tmp/new-private-040r2-s2 \
  --scenario s2 --variant gcc-tsan [--tracer gdb]
```

Variants: `gcc-debug`, `clang-debug`, `gcc-asan`, `gcc-tsan`, `clang-tsan`. Use
`-DCMAKE_CXX_COMPILER=clang++` for Clang and `GIGE_MEL_SANITIZERS=address,undefined`
for ASan/UBSan. The launcher refuses to start if any of these is inherited:
`LD_PRELOAD`, `LD_AUDIT`, `LD_DEBUG`, `LD_DEBUG_OUTPUT`, `TSAN_OPTIONS`,
`ASAN_OPTIONS`, `UBSAN_OPTIONS`, `LSAN_OPTIONS`, `MSAN_OPTIONS`. It then sets the
exact R1 options itself. Exit status is zero **only**
for a complete clean scenario, so a reproduced CHECK exits 1. The
`reproducer` field reports `REPRODUCED_VENDOR_THREAD_LIFECYCLE_CHECK`,
`UNEXPECTED_SUCCESS` or `DIFFERENT_FAILURE`. It is never a qualification pass.

The SDK-free controls:

```sh
cmake -S "$repo/tests/repro/pthread_tsan_controls" -B /tmp/pthread-controls-gcc-tsan \
  -G Ninja -DCMAKE_CXX_COMPILER=g++ -DREPRO_SANITIZERS=thread
cmake --build /tmp/pthread-controls-gcc-tsan
/tmp/pthread-controls-gcc-tsan/pthread_valid_controls create-join         # exit 0
/tmp/pthread-controls-gcc-tsan/pthread_negative_controls detach-after-join # expected CHECK
```
