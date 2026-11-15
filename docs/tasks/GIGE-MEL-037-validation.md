# Register transport qualification

Base commit `cf5c27f725188658f19ade4e0b4fdc3c2adf49fa`, tree
`227ab4649f5708120c45db7b732c77e7d471f2ab`; PR #30 confirmed merged.
Branch `feature/gige-mel-037-navigation-register-transport`.
Final identities and executed counts belong on the delivery PR, avoiding a
self-referential commit identity in this file.

## Reproduction

Linux x86-64; GCC 14.2 / Clang 19.1.7, Debug, CMake/Ninja. Explicitly select
GIGE_MEL_BUILD_ARAVIS, GIGE_MEL_BUILD_PLEORA, GIGE_MEL_SANITIZERS and compiler.
Native pins: Aravis 0.8.36, eBUS 6.5.3.7155, GenApi 3.4.1. External SDK paths
match [prior qualification](GIGE-MEL-035-validation.md).

Build --parallel 2, then full CTest --output-on-failure. Nine lanes: GCC SDK-free
Debug/ASan+UBSan/TSan, Clang SDK-free Debug, Aravis-only Debug, Pleora-only Debug,
combined Debug/ASan+UBSan/TSan. Serialize CTest runs across trees (UDP 3956).
ASan detect_leaks=1:halt_on_error=1; UBSan halt_on_error=1:print_stacktrace=1;
TSan halt_on_error=1. No sanitizer exceptions added/changed. Existing GenApi scoped
audit still requires the exact unsuppressed vendor lock cycle before allowing
its existing single-function exception.

The full suite retains generic/Cognex profiles, independently built modules,
dynamic load/unload, navigation forwarding, streams/leases, cancellation, failure
recovery and SDK buffers. Additional checks: SDK-free/combined installed consumers
and isolation, upstream/JSON integrity, publication safeguards, Markdown links,
whitespace. Executed results are reported separately, never inferred from this
reproduction list or configured CI.

## New evidence

* device_register_contract: exact words/high bits/zero, non-symmetric bytes,
  maximum/end-of-window transfers, empty/oversized/overflow/alignment/address
  refusals before I/O, no partial read output, diagnostics, uncertain writes,
  no retries/rollback, closed/lost refusal and transaction versus concurrent close.
* aravis_register_contract: actual Fake numeric/raw APIs, read-only XML write
  refusal (FALSE without GError), continued use after failure, concurrent close,
  logical close and control-lost admission. Fake is not GVCP evidence.
* aravis_gige_loopback_contract scenario T: production backend, genuine GVCP/GVSP,
  exact 4 KiB raw transfer across native packet boundaries, numeric registers,
  access while acquiring. Existing simulator wildcard discovery listener exception
  remains; client discovery restricted to loopback. No physical camera.
* pleora_register_contract: production call boundary with recording native-shaped
  calls, actual PvResult translation and actual disconnected PvDeviceGEV calls.
  Not connected-camera or GVCP qualification.

## Open technical gates

[Transport semantics](../device-register-transport.md) document SDK-internal
retry, pending ACK, timeout/cancellation and packetization limits. Strict
no-wire-retry and hard bounded execution cannot be claimed using the pinned
Aravis public APIs. Connected Pleora raw transport/concurrency and physical sensor
acceptance remain unqualified. No physical testing was performed. Passing local
fixture/regression checks does not close these acceptance gates; review readiness
must not be asserted while required technical qualification remains open.