// TEST-ONLY ThreadSanitizer suppressions for aravis_gige_loopback_contract
// ONLY (GIGE-MEL-011). The shared tests/support/tsan_uninstrumented_sdk.cpp
// used by the three earlier SDK test executables is UNCHANGED; this
// translation unit replaces it for the loopback executable alone and carries
// the same two patterns plus the investigated GigE-path additions below.
// Consulted only by the TSan runtime.
//
// Investigation (docs/validation.md, GIGE-MEL-011 "TSan"): the same
// executable linked with ONLY the two existing patterns produced 68 reports
// in one full run (halt_on_error=0). The production GigE path adds two SDK
// threads (ArvGvDevice heartbeat, ArvGvStream receiver). Every report has an
// access made by an intercepted libc call from inside the UNINSTRUMENTED
// Aravis/GLib/GIO libraries on at least one side:
//   1. (19 + 1) recv() into the ArvGvDevice GVCP buffer and poll() on its
//      GPollFD, by the heartbeat thread versus the caller's GVCP feature I/O;
//      both run under io_data->mutex in _send_cmd_and_receive_ack (pinned
//      src/arvgvdevice.c:138-180), a GMutex. SDK-internal on both sides.
//   2. (35) eventfd/write/close on the GCancellable descriptors of the
//      heartbeat and receiver threads (g_cancellable_cancel in the SDK
//      finalizer, reached from the project's unique_ptr::reset of the last
//      reference in retire_locked()/close(), versus make_pollfd/release_fd
//      on the SDK thread), serialized by GCancellable's GMutex and the
//      following g_thread_join. SDK-internal on both sides.
//   3. (13) THE SDK BUFFER-HANDOFF BOUNDARY: the ArvGvStream receiver's
//      memcpy() of GVSP payload into the pool buffer (libaravis `_loop`,
//      pinned src/arvgvstream.c:577) versus a PROJECT read of the leased
//      bytes after arv_stream_timeout_pop_buffer() (test pixels_match/inspect,
//      production write_pgm and the provider's destination copy). The buffer
//      is handed over through GAsyncQueue (a GMutex) only after completion;
//      late packets of a completed frame are ignored by the SDK. This is the
//      same ownership boundary the two GIGE-MEL-009 patterns already cover
//      for the Fake stream; on GigE the writer frame is memcpy, so they do
//      not match.
// On Linux a GMutex is a futex with an inline atomic, invisible to TSan.
//
// The added `called_from_lib:` entries make TSan ignore the memory accesses
// and synchronization of intercepted calls made from inside these four
// uninstrumented libraries; they do not suppress reports by stack frame.
// BLIND SPOT (wider than the two patterns, for this executable only): any race
// in which one side is such an SDK-internal libc access is not reported -
// including, as in group 3, the SDK writing a buffer the project reads.
// Project-versus-project races are still reported and halt (verified with an
// intentional-race control linked with this file). A green TSan run of this
// executable is not instrumented-SDK coverage and not race coverage of the
// SDK buffer-handoff boundary; byte-stability of held leases is asserted
// functionally instead (scenarios C, D1, M1).
extern "C" const char* __tsan_default_suppressions();
extern "C" const char* __tsan_default_suppressions() {
    return "race:g_queue_pop_tail\n"
           "race:arv_buffer_set_n_parts\n"
           "called_from_lib:libaravis-0.8.so.0\n"
           "called_from_lib:libgio-2.0.so.0\n"
           "called_from_lib:libglib-2.0.so.0\n"
           "called_from_lib:libgobject-2.0.so.0\n";
}
