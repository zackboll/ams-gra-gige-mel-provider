# Pleora support escalation record — DRAFT, NOT SENT

Status: **prepared for review, not sent.** A maintainer must decide whether and how
to submit it. It contains no SDK binaries, headers, disassembly, license
material, packet captures or private logs. Details:
[investigation](pleora-native-tsan-investigation.md).

---

**Subject:** eBUS SDK 6.5.3.7155 (Linux x86-64): `PvStream::Close()` and
`PvPipeline::Stop()` call `pthread_detach` on a thread they already joined

**Affected version.** eBUS SDK 6.5.3.7155, Ubuntu-22.04-x86_64 package. Library
build IDs:

| Library | Build ID |
| --- | --- |
| `libPtUtilsLib.so.7.5` | `59b45a922e490202719b1a58fccf628b70a03b27` |
| `libPvStream.so.6.5` | `6843b794a8787e3e35aac9714befbc04ea5f8d2b` |
| `libPvBase.so.6.5` | `16dc2e3ab0af8f0efced92626201d67e80ee677b` |

**Runtime.**

* Host: Debian 13, kernel 6.12, glibc 2.41.
* Compilers: GCC 14.2.0 (libtsan.so.2) and Clang 19.1.7 (compiler-rt TSan).

**Minimal reproduction (no PvDevice, PvPipeline or buffers).** The program uses
only documented calls on the main thread, and every result is OK:

```cpp
PvStreamGEV stream;
stream.Open(PvString("10.64.40.2"), 41040, 0, PvString("10.64.40.1"), 16); // OK
stream.Close();                                                             // OK
// destroy
```

The device IP answers discovery (an isolated test network: a virtual Ethernet pair
in a private namespace with a GVCP responder; no physical camera). Build the
program with `-fsanitize=thread`.

**Observed sequence.** The trace used gdb breakpoints on exported glibc
`pthread_create`, `pthread_join` and `pthread_detach`, with arguments and return
values. All three calls below are on the **same `pthread_t`**: the stream receiver
thread, whose start routine is `PtUtilsLib::WorkLink(void*)`.

1. `pthread_create(joinable)` returns **0**, from `PtUtilsLib::Thread::StartThread()`
   ← `PvStreamLib::DataReceiverUM2Thread::StartThread` ← `DataReceiverUM2::Open`
   ← `PvStreamGEV::Open`.
2. `pthread_join` returns **0**, from `PtUtilsLib::Thread::WaitUntilSignaled()`
   ← `DataReceiverUM2Thread::StopThread()` ← `DataReceiverUM2::Close()`
   ← `StreamGEV::Close()` ← `PvStream::Close()`.
3. About 0.3–0.5 ms later, `pthread_detach` is called on the **already-joined
   handle**, also from `WaitUntilSignaled()`.

`PvPipeline::Stop()` shows the same join-then-detach sequence for the pipeline
worker thread, which is created in `PvStreamLib::Pipeline::Start()`.

**Ordinary execution versus ThreadSanitizer.**

* **Ordinary (GCC and Clang builds):** step 3 returns **3 (ESRCH)**. `Close()` and
  `Stop()` return OK, and the program completes.
* **ThreadSanitizer (GCC and Clang):** the interceptor aborts the process before
  the real detach runs, with exit 66:

  ```text
  ThreadSanitizer: CHECK failed: sanitizer_thread_registry.cpp:348 "((t)) != (0)"
    #2 __sanitizer::ThreadRegistry::ConsumeThreadUserId(unsigned long)
    #3 pthread_detach
    #4 PtUtilsLib::Thread::WaitUntilSignaled()      (libPtUtilsLib.so.7.5+0x1d7bc)
    #5 PvStreamLib::DataReceiverUM2Thread::StopThread() (libPvStream.so.6.5+0x3a604)
    #8 PvStream::Close()                             (libPvStream.so.6.5+0x30181)
  ```

  With a pipeline, frame #5 is `PvStreamLib::Pipeline::Stop()`
  (`libPvStream.so.6.5+0x2c75f`). An SDK-free program that calls `pthread_detach`
  after a successful `pthread_join` produces the identical CHECK. Valid
  create/join and create/detach programs do not.

POSIX leaves detaching an already joined thread undefined. With glibc it returns
ESRCH while the descriptor is unused, but the handle could identify another
thread once glibc reuses the descriptor. This prevents ThreadSanitizer
qualification of any application that opens a stream.

**Questions.**

1. Is this join-then-detach in `PtUtilsLib::Thread::WaitUntilSignaled()` known, and
   is it fixed in a later eBUS release or hotfix? If so, which version?
2. Is there a documented shutdown sequence for `PvStreamGEV` and `PvPipeline`
   that avoids it? For example, a different order of `Stop`, `Close`, destruction or
   `AbortQueuedBuffers`, or an option affecting the receiver thread.
3. Can you confirm whether the intended behaviour is to join **or** detach the
   thread, not both?

We have not disassembled or modified the SDK. We can provide the SDK-free pthread
control program and the stage-by-stage test harness source on request.
