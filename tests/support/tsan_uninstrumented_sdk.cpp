// TEST-ONLY ThreadSanitizer suppressions for the UNINSTRUMENTED Aravis SDK
// and the system GLib it uses (GIGE-MEL-009). Linked only into the two test
// executables that drive a real SDK receiver thread (aravis_stream_contract
// and aravis_mel_embedding); consulted only by the TSan runtime.
//
// Why: Aravis hands buffers between its receiver thread and the caller
// through GAsyncQueue. On Linux, GLib's g_mutex_lock is a futex with an
// inline atomic compare-exchange, not pthread_mutex_lock, so TSan cannot see
// that synchronization. It does intercept malloc/free/memset called from
// those libraries (queue nodes in g_queue_pop_tail, ArvBuffer part tables in
// arv_buffer_set_n_parts) and reports those correctly locked SDK handoffs as
// races. `race:` suppressions only filter reports whose stack contains the
// named SDK function; they do not change lock tracking, and every access by
// the project's own instrumented code (adapter, provider, tests), including
// reads of leased pixel bytes, is still checked with halt_on_error.
// TSAN_OPTIONS, presets and CI lanes are unchanged. Project sanitizer
// coverage is not instrumented SDK coverage.
extern "C" const char* __tsan_default_suppressions();
extern "C" const char* __tsan_default_suppressions() {
    return "race:g_queue_pop_tail\n"
           "race:arv_buffer_set_n_parts\n";
}
