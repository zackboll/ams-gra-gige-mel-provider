# Documentation

This is the canonical navigation entry point for the native C++20 AMS-GRA IR
MEL **provider**. Start with the [README](../README.md) for the SDK-free build
and the [current capability and evidence status](status.md) for scope and limits.
MEL means MFA Encapsulation Layer, not Matrox MIL.

## Use and integrate

| Need | Start here |
| --- | --- |
| Build and capture without a camera SDK | [README quick start](../README.md#sdk-free-quick-start), [native capture example](../examples/capture.cpp) |
| Understand supported operations and lifecycle | [Implementation profile](implementation-profile.md), [configuration](configuration.md) |
| Understand loading, installation and interface boundaries | [Architecture](architecture.md), [installed consumer example](../tests/install_consumer/) |
| Integrate Aravis | [Aravis integration and timing gates](aravis-integration.md) |
| Integrate Pleora | [Pleora integration, licensing and evaluation limits](pleora-integration.md), [consolidation disposition](pleora-consolidation.md) |
| Select a compiled backend or explicit profile module | [Internal runtime selection](runtime-selection.md) |
| Use the Cognex-specific profile | [Module build/admission guide](../camera_profiles/cognex_cic_5000r/README.md), [physical camera record](cognex-cic-5000r.md) |

## Develop

1. Read the [architecture](architecture.md) and [repository instructions](../AGENTS.md).
2. For a **new camera model on an existing SDK**, start with
   [camera-profile authoring](camera-profile-authoring.md). Implement
   [`ICameraProfile` and its owned state](../src/profiles/camera_profile.hpp)
   using the typed [`IDeviceFeatures`](../src/device_features.hpp) service.
   The [independent module example](../examples/profile_module/README.md) and
   [Cognex module](../camera_profiles/cognex_cic_5000r/README.md) show build and
   admission patterns. Do not add SDK policy branches to the MEL facade.
3. For a **new SDK/transport**, start with [backend authoring](backend-authoring.md),
   then [backend portability](backend-portability.md) and
   [camera-profile portability](camera-profile-portability.md).
4. For **MEL implementation work**, use the
   [complete MEL surface inventory and requirements](mel-surface-roadmap.md)
   together with the exact pinned [IR MEL declarations](../third_party/ir-mel/include/)
   and [Common MEL declarations](../third_party/common-mel/include/).
   The [implementation profile](implementation-profile.md) describes current
   behavior; it is not a substitute for the complete upstream requirements.
5. Run the SDK-free development suite from the [quick start](../README.md#sdk-free-quick-start).
   The [CMake definitions](../CMakeLists.txt), [presets](../CMakePresets.json) and
   [CI workflow](../.github/workflows/ci.yml) are the executable build/test
   configuration. SDK-enabled and sanitizer results require their own executed
   evidence; configured CI lanes alone are not qualification.

### Behavior references

- Images and memory: [Mono12](mono12-image-profile.md),
  [source-backed stacks](stacked-image-provider.md),
  [Image LOS](image-los-metadata.md).
- Commands and software state: [ModeCmd](mode-command.md),
  [Health](health-status.md), [software BIT](software-bit.md),
  [subsystem status](subsystem-status.md).
- Navigation: [reception](navigation-reception.md),
  [optional backend forwarding](navigation-backend-forwarding.md).
- Internal transport work: [register/memory service](device-register-transport.md),
  [controlled GVCP experiment](controlled-gvcp-transport.md),
  [loopback single-owner experiment](gvcp-single-owner.md).

## Evidence and design history

Historical records retain their original scope, test counts and conclusions.
Use [current status](status.md) to distinguish today's implementation from those
milestone snapshots; a design proposal is not proof of implementation.

- [Validation record](validation.md) and [task design/validation records](tasks/).
- [Cognex physical observations](cognex-cic-5000r.md),
  [GVSP receive-buffer measurements](gvsp-receive-buffer.md),
  [inconclusive packet-spacing experiment](gvsp-packet-spacing.md).
- Pleora: [stream-only audit](pleora-stream-only-audit.md),
  [native TSan investigation](pleora-native-tsan-investigation.md),
  [scoped TSan exception](pleora-tsan-exception.md),
  [vendor escalation](pleora-tsan-vendor-escalation.md),
  [GenApi lock-order investigation](genapi-tsan-lock-order.md).
- Provider teardown: [TSan investigation](provider-thread-exit-tsan.md),
  [thread-exit stacks](provider-thread-exit-stacks.md).
- GenTL: [probe design](tasks/GIGE-MEL-GENTL-001-design.md),
  [backend fixture results](tasks/GIGE-MEL-GENTL-002-validation.md),
  [producer qualification results](tasks/GIGE-MEL-GENTL-003-validation.md),
  [wire harness results](tasks/GIGE-MEL-GENTL-003A-validation.md).
- [Native C++ provider decision](adr/0001-native-cpp-provider.md),
  [copy-first memory decision](adr/0002-copy-first-memory.md),
  [upstream provenance](upstream-provenance.md).

## Documentation follow-up

The following are remaining gaps, not implemented features or claims about the
scope of future tickets. Suggested allocation for DOC-GIGE-002 through
DOC-GIGE-006:

| Campaign item | Remaining documentation work |
| --- | --- |
| DOC-GIGE-002 | Doxygen/GitHub Pages generation, publication and source-link/navigation validation; separate public headers from internal developer references. No pipeline or generated site is included here. |
| DOC-GIGE-003 | Consolidated installed-consumer integration guide: loading, CMake package, callback/release/teardown examples and error handling. |
| DOC-GIGE-004 | End-to-end camera-profile tutorial with minimal implementation, independent matched-build module and qualification checklist. |
| DOC-GIGE-005 | MEL requirements-to-implementation/test cross-reference; distinguish required gaps, conditional support and pinned annotation/enum blockers. |
| DOC-GIGE-006 | Evidence index by SDK/camera/firmware and environment, reproducible validation commands, plus a broader stale-text/link audit of milestone-heavy references. |