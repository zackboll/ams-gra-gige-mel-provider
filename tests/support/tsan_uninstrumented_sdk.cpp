// TEST-ONLY ThreadSanitizer suppressions for the UNINSTRUMENTED Aravis SDK
// and the system GLib it uses (GIGE-MEL-009). Linked only into the test
// executables that drive a real SDK receiver thread (aravis_stream_contract,
// aravis_mel_embedding and, since GIGE-MEL-010, aravis_timestamp_contract);
// consulted only by the TSan runtime. The two patterns are unchanged.
//
// Why: Aravis hands buffers between its receiver thread and the caller
// through GAsyncQueue. On Linux, GLib's g_mutex_lock is a futex with an
// inline atomic compare-exchange, not pthread_mutex_lock, so TSan cannot see
// that synchronization. It does intercept malloc/free/memset called from
// those libraries (queue nodes in g_queue_pop_tail, ArvBuffer part tables in
// arv_buffer_set_n_parts) and reports those correctly locked SDK handoffs as
// races (63 unsuppressed reports in the retained GIGE-MEL-009 run: 45 with
// g_queue_pop_tail, 18 with arv_buffer_set_n_parts; see docs/validation.md).
//
// What these patterns do NOT preserve (GIGE-MEL-009-R1 correction): a
// `race:` pattern suppresses the WHOLE report when the named function
// appears in ANY frame of EITHER report stack. Project code stays
// instrumented, but a report in which project code participates - e.g. a
// project access racing with an access whose stack passes through one of
// these GLib/Aravis frames - is suppressed too. This is a specific reporting
// blind spot at the SDK buffer-handoff ownership boundary: a green TSan run
// with these patterns is NOT unrestricted race coverage of that boundary.
// The review's intentional-race control demonstrated these suppression
// semantics; it did not discover a production adapter race. Other project
// reports still halt with halt_on_error. TSAN_OPTIONS, presets and CI lanes
// are unchanged. Project sanitizer coverage is not instrumented SDK coverage.
extern "C" const char* __tsan_default_suppressions();
extern "C" const char* __tsan_default_suppressions() {
    return "race:g_queue_pop_tail\n"
           "race:arv_buffer_set_n_parts\n";
}
