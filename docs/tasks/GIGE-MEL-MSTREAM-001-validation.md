# GIGE-MEL-MSTREAM-001 — Qualification

- Starting and final base: `9593bde07d7451d325326074e797132590566ece`.
- Tested executable source: `215f5ad4641d1637da47f1c4162d402683375253`,
  tree `c8cd4274321c213d8143c79769186bf869f98496`. Subsequent changes are
  documentation only.
- Delivery: [PR #68](https://github.com/zackboll/ams-gra-gige-mel-provider/pull/68).

## SDK-free concurrency and regression

`synthetic_multistream` opens one device, enumerates a passive, validated
topology, and starts two receivers in reverse selection order. It checks
exact pixels, source stride and span, clock provenance, independent frame IDs
and periods, and restart generations. Holding the only Stream 0 slot does
not starve Stream 1. Restart fails while that source is pinned; after release
it can join an existing acquisition without changing its peer's generation.
A pinned Stream 1 pool induces a second-source preparation failure after Stream
0 was staged, and the next start proves no leaked admission or consumed
generation. Teardown tests cancel one blocked receive, race repeated stop and
close while the other receive is blocked, reopen the device, and verify both
leases still read correctly after the old device and receivers are gone.

The MEL regression also rejects wrong source or product keys on the singleton
Image path without changing existing timestamp admission or channel discovery.

| Local Linux x86-64 configuration | Result |
| --- | --- |
| GCC 14.2 Debug | 61/61 passed |
| GCC 14.2 Release | 61/61 passed |
| Clang 19.1.7 Debug | 61/61 passed after rerunning a compiler output I/O failure |
| GCC ASan/UBSan, leak detection and halt-on-error | 61/61 passed |
| GCC ThreadSanitizer, halt-on-error, no suppressions | 61/61 passed; `synthetic_multistream` also passed 20 consecutive independent CTest runs |

Each full lane used `GIGE_MEL_TEST_CONTROL_AUTHORITY=ON` and the matching CMake
preset (Debug with `CXX=clang++` for Clang). Focused reproduction:

```sh
cmake --preset tsan -DGIGE_MEL_TEST_CONTROL_AUTHORITY=ON
cmake --build --preset tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan \
  -R '^synthetic_multistream$' --repeat until-fail:20
```

## Native fixture scope

The combined Debug suite passed **135/135**. It uses pinned Aravis 0.8.36
(including Fake and loopback), Pleora eBUS 6.5.3.7155 with matching GenApi
3.4.1 headers, and the externally
provisioned EMVA GenTL 1.6 header with the controlled mock producer. The focused
Pleora TSan run passed 3/3 (SDK, register and MEL contracts); the SDK contract
alone used the existing fingerprinted and independently audited
[GenApi exception](../pleora-tsan-exception.md). This is not an unsuppressed
qualification of the external GenApi runtime.

No physical camera, vendor GenTL producer, or native multi-stream receiver was
operated. SDK fixture passes only cover existing paths, not the native
multi-stream sequencing or device/profile requirements in the
[feasibility audit](GIGE-MEL-MSTREAM-001-design.md#native-feasibility-pinned-apisource-inspection-not-execution).

## Installed consumer and documentation

- Fresh SDK-free Release and combined Debug installs passed the standalone
  capture example, ordinary MEL consumer, and installed stack and task
  consumers. Dynamic `mel_capture` produced a 64 × 48 Mono8 frame.
- The MEL-only executable built and linked against the exact base ran against
  the new Release library; `ldd` confirmed that substitution. It does not
  establish binary compatibility for project backend adapters. SONAME,
  Release direct dependencies, factory symbol names and header/library/package
  paths were unchanged. Of 120 installed headers only `backend.hpp` changed;
  it compiled standalone with both compilers. The combined install passed
  `verify_package_isolation.py` and added no optional SDK dependency to the
  installed provider.
- The loader rejected a Cognex module built from the exact base with
  `incompatible C++ build identity` and accepted a freshly matched module.
  Project C++ adapters/embedders and matched-C++ modules must rebuild; pinned
  MEL declarations and factory signatures are unchanged.
- Pinned upstream and JSON byte verification, proprietary index/history guard,
  `git diff --check`, traceability validation, evidence-index negative controls,
  and pinned Doxygen 1.14.0 site validation passed. The initial status-table
  edit violated the site's fixed inventory check; the corrected document passed.
  The only Doxygen warning is the existing `with_registers` overload warning.