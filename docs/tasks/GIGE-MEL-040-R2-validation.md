# GIGE-MEL-040-R2 validation record

## Source and environment

* **Base:** fetched `main` `d49f2ed531f7a59323f8e93e0938f29849241df1`, tree
  `7b674057c5ab37a45329b0a022b5ea179db9e3f9`. PR #35 is merged.
* **Branch:** `feature/gige-mel-040-r2-pleora-tsan-stop`.
* **Qualified source commit:** `9a622483574ba3ac36548a826744415ccaa2dbb3`, tree
  `ab683d57badf59d93bb477e7f702595475af85a4`. The worktree was clean when the
  matrix started.

This record is committed afterwards as a documentation-only change, so it does
not name its own commit. The delivery PR reports the final head and confirms that
the later diff touches documentation only.

**Host and toolchain:**

* Debian 13.7 on kernel 6.12.116, x86-64.
* glibc `libc6 2.41-12+deb13u4`, libc build ID `5e5b51f4…73cc`.
* GCC `14.2.0-19`, with `libtsan2 14.2.0-19`:
  `libtsan.so.2.0.0` SHA-256 `d4d8d0b7…30d4`, build ID `99ef8859…1624`.
* Clang `1:19.1.7-3+b1`, with static `libclang_rt.tsan-x86_64.a` SHA-256
  `4ebde9fd…78ca`.
* gdb `16.3-1`, CMake 3.31.6, Ninja, Python 3.13.5.

**SDKs and pinned dependencies:**

* External eBUS **6.5.3.7155** (Ubuntu-22.04-x86_64 package), with GenApi 3.4.1
  headers. Loaded build IDs: `libPtUtilsLib.so.7.5` `59b45a92…7b27`,
  `libPvStream.so.6.5` `6843b794…2b`, `libPvBase.so.6.5` `16dc2e3a…677b`.
* Pinned external Aravis 0.8.36 (S6/D2 fixture and Aravis lanes only).

No physical camera, hardware test option or production camera path was used.

## Native stage matrix (explicit opt-in, private evidence)

The matrix uses five variants, built with
`-DGIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT=ON`:

* GCC Debug, binary `9909dd48…`;
* Clang Debug, binary `b2315307…`;
* GCC ASan/UBSan, binary `2810856e…`;
* GCC TSan, binary `630f7503…`;
* Clang TSan, binary `1c33270f…`.

The binaries are unchanged between `matrix1` and the final S2 reruns. Each
variant ran 11 scenarios, in 55 fresh isolated processes. The results are in the
[investigation](../pleora-native-tsan-investigation.md#scenario-matrix):

* 33 ordinary and ASan/UBSan runs completed;
* 20 TSan runs (S2–S8, D1, D2) ended in the registry CHECK;
* 2 TSan S1 runs gave the known log4cpp lock-order report;
* the 2 TSan S0 runs passed.

All 55 runs had runtime identity consistent, packet observers healthy, cleanup
verified, owned children and namespaces retired, and an unchanged host network
inventory.

**Minimal S2 reproducer on the final source** (`/tmp/gige040r2-evidence/s2-final`):

| Variant | Native exit | Stages | Disposition | `reproducer` |
| --- | --- | --- | --- | --- |
| GCC TSan | 66 | 4/7, last `before_close` | FATAL_SANITIZER_CHECK | REPRODUCED_VENDOR_THREAD_LIFECYCLE_CHECK |
| Clang TSan | 66 | 4/7, last `before_close` | FATAL_SANITIZER_CHECK | REPRODUCED_VENDOR_THREAD_LIFECYCLE_CHECK |
| GCC Debug | 0 | 7/7, Open OK, Close OK | SCENARIO_COMPLETE | not a TSan reproducer |
| Clang Debug | 0 | 7/7, Open OK, Close OK | SCENARIO_COMPLETE | not a TSan reproducer |
| GCC ASan/UBSan | 0 | 7/7, Open OK, Close OK, no diagnostic | SCENARIO_COMPLETE | not a TSan reproducer |

gdb-traced S2 (GCC/Clang Debug and TSan) shows complete traces and the
create(0) → join(0) → detach sequence on one `pthread_t`. The detach returns
ESRCH ordinarily and never returns under TSan. Values and timestamps are in the
investigation document.

**One superseded run.** The first final-source Clang TSan gdb run was
characterized `DIFFERENT_FAILURE`. gdb's `[Detaching after fork …]` notice landed
between the CHECK line and its stack. The fail-closed parser correctly refused to
infer the stack. Fix: the tracer now disables gdb inferior-event notices, and a
regression test retains the split-stack case as non-characterized. All four gdb
variants were rerun. That run is retained as
`superseded-s2-clang-tsan-gdb-split-stack`; it is not counted.

The R1 binary (`6e482831…ce0eb`) and a fresh base build both reproduce the
original `PvPipeline::Stop()` CHECK unchanged.

## SDK-free checks

* **Orchestration tests** (`tests/pleora_tsan_stage_audit_tests.py`): 27 tests
  pass, and they are registered as `pleora_tsan_stage_audit_tooling_contract` in
  ordinary CTest. Among other things, the tests reject:
  * an unexpected successful return of the S2 reproducer;
  * zeroed or missing exit status;
  * forged completion markers;
  * altered CHECK line, frame or stage;
  * mixed diagnostics;
  * incomplete gdb traces (missing ready, exit status or finish);
  * inherited sanitizer or loader environment;
  * native-audit CTest registration or installation;
  * any widening of the GenApi suppression.
* **Mutation controls** (private driver, sources restored and hash-verified): the
  baseline passes. Each of 11 deliberate weakenings is detected by a failing test:
  * fatal CHECK not a diagnostic;
  * nonzero exit ignored;
  * completion marker not required;
  * unexpected reproducer success relabelled;
  * reproducer accepts zero exit;
  * reproducer reported as pass;
  * incomplete trace accepted;
  * trace completeness skips exit;
  * failed join accepted as join-then-detach;
  * negative control accepts zero exit;
  * inherited `TSAN_OPTIONS` accepted.
* **pthread controls** (rebuilt from committed source, 24 runs): GCC and Clang,
  each TSan and uninstrumented. All valid controls exit 0 with no diagnostic. All
  six TSan negative runs are characterized `EXPECTED_FATAL_CHECK` (exit 66). The
  uninstrumented negative runs return EINVAL/ESRCH as tabulated in the
  investigation. They remain expected-failure experiments outside CTest.

## Final regression (nine lanes, native audit OFF)

`ctest --output-on-failure --parallel 1`, run serially across lanes, Debug,
`build --parallel 2`. Options: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, `TSAN_OPTIONS=halt_on_error=1`.
Inherited `LD_*`/`*SAN_OPTIONS` variables were removed first. Hardware options were
OFF.

| Lane | Compiler | Aravis | Pleora | Sanitizers | Passed / failed / total |
| --- | --- | --- | --- | --- | --- |
| gcc | GCC | OFF | OFF | none | 29 / 0 / 29 |
| asan | GCC | OFF | OFF | address,undefined | 29 / 0 / 29 |
| tsan | GCC | OFF | OFF | thread | 29 / 0 / 29 |
| clang | Clang | OFF | OFF | none | 29 / 0 / 29 |
| aravis | GCC | ON | OFF | none | 37 / 0 / 37 |
| pleora | GCC | OFF | ON | none | 35 / 0 / 35 |
| combined | GCC | ON | ON | none | 43 / 0 / 43 |
| combined-asan | GCC | ON | ON | address,undefined | 43 / 0 / 43 |
| combined-tsan | GCC | ON | ON | thread | 43 / 0 / 43 |

All configure, build and test steps exited 0, with no skipped or not-run tests.

Compared with the R1 final matrix, each lane has exactly one added test,
`pleora_tsan_stage_audit_tooling_contract`. That test ran and passed in every
lane, and none was removed. No lane registered the native
`pleora_tsan_stage_audit` executable or the pthread controls.

The existing scoped GenApi audit (`pleora_sdk_contract` in combined-tsan) was also
rerun separately. All four gates passed:

* the pinned runtime fingerprints;
* the unsuppressed eight-group exact-cycle audit;
* the exact locked vendor report;
* the deadlock-only exception, with no other TSan findings.

## Packaging, integrity and publication

* **Installed consumers.** GCC, Clang, Pleora-only and combined lane builds were
  each installed, then a consumer was configured, built and run. All steps exited 0.
  * Each install has 172 files, with no audit executable, audit or trace script, or
    pthread control installed.
  * `ldd` of the installed `libgige_mel_provider.so` lists no PvStream, PvDevice,
    PvBase, PtUtils, GenApi or Aravis library.
* **Package/dependency isolation** (`verify_package_isolation.py`, Aravis and
  combined): **verified** for both.
  * Each prefix has 199 installed paths, with only the documented exception.
  * 98 non-documentation text files and 2 binaries are Aravis-free.
  * The consumer was built without `PKG_CONFIG_PATH`.
  * The first driver attempt exited 1 because it passed an *uninstalled* prefix
    (a driver error, not a product finding). Its logs are retained. The rerun
    installed first, as R1 did.
* **Integrity:** 97 pinned upstream files verified, and nlohmann/json 3.12.0
  (2 files) verified, both offline.
* **Publication history guard:** `check_proprietary.py --history origin/main` found
  no prohibited material in 332 Git entries.
* **Documentation links:** 324 relative links (and anchors) in 62 tracked Markdown
  files resolve.
* **Whitespace:** `git diff --check origin/main...HEAD` is clean.
* **Production scope:** the source diff against `origin/main` adds 2232 lines in
  13 files and changes nothing under `src/`, `include/`, `third_party/`, `tools/`
  or `tests/support/`.

## Resource retirement

Every native run's `outer-cleanup.json` shows the coordinator and camera worker
retired, both network namespaces absent from `lsns`, and the host network
inventory unchanged. After all runs:

* no audit, gdb, Aravis or control process remained;
* the host had no veth interfaces;
* only one packet socket existed, a pre-existing NetworkManager ARP socket on
  `enp5s0`, not test-owned.

Private captures and logs remain under `/tmp/gige040r2-evidence/` and are not
approved for publication.

## Dispositions

* **`READY_FOR_INDEPENDENT_REVIEW`**: diagnostic tooling and root-cause evidence
  only.
* **`NATIVE_TSAN_BLOCKED`**: genuine Pleora Open/Close and `Pipeline.Stop` still
  fail TSan (vendor join-then-detach; fatal registry CHECK). This is not
  suppressed, waived, skipped or relabelled.
* **`PRODUCTION_INTEGRATION_BLOCKED`**: licensing (1538 NO_LICENSE), receiver
  policy, controller coexistence and strict additional-GVCP requirements remain
  unresolved.

The vendor escalation record is prepared and **not sent**:
[Pleora escalation record](../pleora-tsan-vendor-escalation.md).
