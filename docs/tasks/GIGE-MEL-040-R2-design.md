# GIGE-MEL-040-R2 pre-implementation design

Base: fetched `main` commit `d49f2ed531f7a59323f8e93e0938f29849241df1`,
tree `7b674057c5ab37a45329b0a022b5ea179db9e3f9`. PR #35 (GIGE-MEL-040-R1) is
merged at that commit. Branch `feature/gige-mel-040-r2-pleora-tsan-stop`.
Written **before** any implementation or new native execution. The retained R1
TSan binary `build/040-audit-tsan/pleora_stream_only_audit` still has the R1
recorded SHA-256 `6e48283125e33443578b004a4bd71c8a3f0f52a370075f6fb2dc1910ba8ce0eb`;
the private R1 evidence under `/tmp/gige040r1-evidence/` is read-only input.

## Preserved R1 finding (not reclassified)

Native stream-only Open/Close works on the isolated veth pair. A genuine
PvBuffer arrives with operation **1538 NO_LICENSE**. The unsuppressed GCC 14.2
TSan run (`halt_on_error=0:exitcode=66:second_deadlock_stack=1`) dies after
`STAGE before_pipeline_stop` with:

```text
ThreadSanitizer: CHECK failed: sanitizer_thread_registry.cpp:348 "((t)) != (0)"
  ThreadRegistry::ConsumeThreadUserId <- pthread_detach (tsan_interceptors_posix.cpp:1094)
  <- PtUtilsLib::Thread::WaitUntilSignaled() (libPtUtilsLib.so.7.5+0x1d7bc)
  <- PvStreamLib::Pipeline::Stop() <- PvPipeline::Stop() <- main:128
```

Native exit 66; no completion marker. That remains a failing native run.

## Reviewed sanitizer mechanism (source-backed, not yet a diagnosis)

GCC 14.2 `libsanitizer` and LLVM 19.1.7 `compiler-rt` have byte-identical
`sanitizer_thread_registry.cpp`. `pthread_detach`/`pthread_join` interceptors call
`ConsumeThreadUserId(pthread_t)` **before** the real libc call. Line 348 is
`CHECK(t)` after `live_.find(user_id)`. Thus the CHECK means: at the moment of
the intercepted detach, the runtime had no live registry entry for that
`pthread_t`. Entries are inserted only by the intercepted `pthread_create`
(`CreateThread`) or `ThreadNotJoined`, and removed by a prior consume
(detach/join), by `FinishThread` of an already-detached thread, or by `OnFork`.
A CHECK is fatal; it is not a report that `halt_on_error`/suppressions govern.

## Hypotheses to discriminate (none presumed)

| ID | Hypothesis | Discriminating observation |
| --- | --- | --- |
| H1 | Project harness misuse (buffer retained, receive active, wrong ordering) | Harness audit against headers/samples; minimal S4 idle Start/Stop without receive or buffers |
| H2 | SDK detaches a `pthread_t` it already detached or joined | Debugger breakpoints on exported glibc `pthread_create/join/detach` with `pthread_t` values and return codes in an uninstrumented run |
| H3 | SDK detaches a thread created invisibly to TSan (e.g. clone, or an unintercepted path) | Same trace: is the detached `pthread_t` ever returned by an observed `pthread_create`? |
| H4 | Detach after the thread exited and its (detached) record retired | Ordering of exit/detach events for that `pthread_t`; registry semantics |
| H5 | `pthread_t` reuse confuses identity | Same `pthread_t` value across distinct create events |
| H6 | TSan runtime defect/limitation independent of the SDK | SDK-free controls; GCC/Clang comparison; whether valid lifecycles ever CHECK |
| H7 | Interaction: uninstrumented SDK + interceptors | Whether the stack/thread transitions differ between instrumented and ordinary executions |

The LLVM issue #156829 (AArch64, Clang 19.1.7, same library frame
`PtUtilsLib::Thread::WaitUntilSignaled`) shows a double-detach reproducer. It is
recorded as related reporting only; it is not proof for GCC/x86-64.

## Evidence policy and limits

The installed eBUS EULA states the buyer “may not reverse compile or disassemble
the software”. Therefore **no vendor disassembly/decompilation** is performed.
Permitted observation: exported symbol tables, runtime syscall tracing, debugger
breakpoints on *glibc* exported functions with argument/return inspection, and
TSan's own diagnostics. No interposition (`LD_PRELOAD`/`LD_AUDIT`), no binary
patching, no license or watermark change, no suppression, no TSan option that
masks the CHECK. Where only vendor internals could decide, that limit is recorded.

## Planned implementation (test-only)

1. Reproduce with the unchanged R1 binary and launcher into new private output.
2. New default-OFF, uninstalled `pleora_tsan_stage_audit` executable (behind the
   existing `GIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT` option) with scenarios S0–S8
   and a separate direct-stream control D1; every SDK result checked; explicit
   `REACHED`/`COMPLETE` markers.
3. New `scripts/pleora_tsan_stage_audit.py` reusing R1 namespace/veth/capture/
   cleanup code; it never maps a nonzero, missing marker, fatal CHECK or
   unexpected diagnostic to success.
4. SDK-free pthread controls (`tests/repro/pthread_tsan_controls/`): valid
   create/join, create/detach, completion-before-join; separate negative
   executables for repeated detach and detach-after-join.
5. GCC vs Clang TSan; uninstrumented GCC/Clang Debug and ASan/UBSan comparisons.
6. SDK-free orchestration regression tests registered in ordinary CTest.

Production Pleora stream teardown is **not** changed unless independently shown
defective. Ordinary CTest/CI never depends on the SDK or a camera.

## Outcome (recorded after execution)

* Results:
  [investigation](../pleora-native-tsan-investigation.md) and
  [validation](GIGE-MEL-040-R2-validation.md).
* H2 is confirmed in the form "detach after a successful join" on the same
  `pthread_t`.
* H1, H3, H4, H5 and H6 are rejected. H7 is not supported.
* A direct `PvStream` control was added as D2 (genuine frames) alongside the
  planned D1.
* The gdb trace was tightened during R2:
  * trace completeness is now gated;
  * gdb inferior-event notices are suppressed, because they split a CHECK line
    from its stack;
  * a characterized `reproducer` status was added.

  None of these changes alters the fail-closed pass criterion.
