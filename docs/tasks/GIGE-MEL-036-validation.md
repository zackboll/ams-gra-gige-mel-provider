# Cognex CIC-5000R profile qualification

PR #29 confirmed merged via GitHub and fetched `origin/main`.
Starting commit `ae44637e8e2bff9c18076d4514c4381c3d561730`, tree
`39af59bfe63cac10d8bde0b1d6baee6defc52ced`.
Branch `feature/gige-mel-036-cognex-cic-5000r-profile`.
Final exact commit/tree and executed results are recorded on the delivery PR;
this document is a reproduction/coverage record, not a claim of unexecuted passes.

## Implementation boundary

The [independent module](../../camera_profiles/cognex_cic_5000r/README.md)
owns a distinct Cognex state and composes SDK-free monochrome algorithms.
Exact three-feature identity is checked before configuration discovery. Initial
policy is Mono8 and 1/2/4/5/8/10 Hz, not an assertion of physical limitations.
Native snapshot bounds remain unmodified so readback and recovery remain truthful.
No provider, SDK adapter, profile interface, module API, pinned upstream bytes,
stream/lease ownership, production timestamp or factory changes.

## Fixture coverage (not camera measurements)

* `cognex_profile_contract`: real documented identity, geometry and numeric bounds
  in typed features; wrong/missing/unreadable/failed identity; disabled/missing
  required acquisition state; Mono12 refusal; all six admitted rates; unsupported
  rates including 12/14/16 before writes; truthful native 16 Hz readback and explicit
  recovery; exposure quantization; partial native write failure, actual resulting
  state, no rollback and recovery; generic capacity validation; unchanged odd/even
  IDs; independently owned sessions/states; profile retirement before state use;
  borrowed feature destruction; twenty actual mapping load/unload cycles.
* `aravis_adapter`: dynamic Cognex module through production Features over native
  Fake GenICam nodes. Exact identity, wrong identities, zero-write unsupported
  rates, six admitted rates/readback, 16 Hz start refusal and native query error
  provenance. Original test-only DOM arrangement retains Fake period conversion;
  it is not copied camera XML. Fake sensor geometry remains the SDK's own geometry.
* `aravis_stream_contract`: same module through production native receive, genuine
  Fake frames/native IDs and retained lease bytes after teardown. Existing generic
  module lifetime/full-pool/failure/cancellation tests are retained.
* `pleora_sdk_contract`: same binary path in combined builds through real GenApi
  production Features with original local-port node map, documented Cognex
  identity/geometry/bounds; wrong identities, zero-write unsupported rates, six
  admitted rates/readback, native 16 Hz preflight refusal, native read/write failure
  provenance and partial-state diagnostics. Real PvBuffer copy stream admits
  consecutive IDs after profile retirement and preserves retained copy bytes.
  New cases run inside the existing audited shared-profile group so all eight
  groups and the scoped native TSan audit remain intact.

No fixture is a connected PvDeviceGEV, camera transport, licensing substitute or
physical-camera evidence. The same module is compiled once per host build and
loaded by both native SDK test processes, never linked into them.

## Reproduction matrix

Linux x86-64, GCC 14.2.0, Clang 19.1.7/libstdc++, CMake/Ninja Debug.
Configure `build/036-LANE` with explicit `GIGE_MEL_BUILD_ARAVIS`,
`GIGE_MEL_BUILD_PLEORA`, `GIGE_MEL_SANITIZERS`; build `--parallel 2`; full
`ctest --test-dir /absolute/checkout/build/036-LANE --output-on-failure`.

| Lane | Aravis | Pleora | Sanitizer |
| --- | --- | --- | --- |
| free-dev | OFF | OFF | none |
| free-asan | OFF | OFF | address,undefined |
| free-tsan | OFF | OFF | thread |
| clang-free | OFF | OFF | none, Clang |
| aravis-dev | ON | OFF | none |
| pleora-dev | OFF | ON | none |
| both-dev | ON | ON | none |
| both-asan | ON | ON | address,undefined |
| both-tsan | ON | ON | thread |

Native test trees run sequentially (shared simulator UDP port 3956).
An exploratory ASan run caught a fixture-only Aravis DOM rename use-after-free:
registered feature names are borrowed by the SDK's hash table. The fixture now
adds a separate Abs node linked to the original period converter and marks the
original alias unimplemented, without renaming any registered node. The final
matrix is rerun on the corrected exact HEAD; no sanitizer/test weakening.
Use `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`,
`TSAN_OPTIONS=halt_on_error=1`. The unchanged scoped native GenApi audit first
requires the exact unsuppressed vendor lock cycle, then permits only its existing
single-function exception; no new suppression. Proprietary SDKs are uninstrumented.
External paths match [the preceding qualification](GIGE-MEL-035-validation.md).

Every test build configures/builds the module independently via ExternalProject.
Also build it standalone; check sole export, DT_NEEDED isolation and absence of
NODELETE, then select through both SDKs without `--inspect` (no hardware I/O).
Additional checks: SDK-free and combined installed consumers, package isolation,
upstream and JSON integrity, index/new-history publication guard, local Markdown
links, and `git diff --check`. No binaries, SDK material or camera images committed.

## Physical observations and remaining gates

No new physical testing is authorized or required. Prior observations only are
in the [camera record](../cognex-cic-5000r.md). Full-frame Mono8 at 2/5/10 Hz on
the observed 1 Gb/s link is the strongest historical evidence; heavy-load drops
preclude loss-free guarantees. 1/4/8 Hz, ROI, native Mono12 byte order, new firmware,
production clocks, Pleora development entitlement and production qualification
remain unqualified. The new module itself is source/fixture-qualified only.