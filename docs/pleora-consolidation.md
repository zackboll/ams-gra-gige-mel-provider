# GIGE-MEL-027 — one canonical Pleora backend

Canonical PR: [#26](https://github.com/zackboll/ams-gra-gige-mel-provider/pull/26),
branch `feature/gige-mel-025-pleora-backend`. Competing
[PR #25](https://github.com/zackboll/ams-gra-gige-mel-provider/pull/25) must not
be merged alongside it. This corrective work does not merge either PR.

## Immutable starting identities

Fetched before edits on 2026-11-13 (host local date):

| Source | Commit SHA | Tree SHA |
| --- | --- | --- |
| main | `c546cd342338dc662efbaf08ad524b120c280c34` | `41202675eb373d70e01f20f8a960b99ca915f6b7` |
| PR #25 | `20dce98e7e1a119d9fce500f3f77bd83dbd92a83` | `e72bc7c3b817e5b0273b01bb9a821f956ee4b760` |
| PR #26 | `fb473dc37389074ead962b5da5b3ad874b951a6a` | `4ea55734b59fa471bb6b062445c75e65360b7d6c` |

## Source/contract comparison and disposition

Classification describes starting source, not PR prose. Both complete diffs,
including build scripts and documentation, were reviewed. Execution evidence is
separated from source review; historical acquisition is not final-HEAD testing.

| Capability | Classification | Source evidence and consolidated disposition |
| --- | --- | --- |
| Discovery / exact identity | Equivalent core; potentially deficient edge handling | Both `pleora_backend.cpp` select discovered MAC, never IP fallback. #25 prefixes `mac:`, canonicalizes discovered identities and rejects ambiguous paths; #26 uses bare lower-case MAC and deduplicates discovery. Preserve #26 identity contract; malformed/disagreeing multi-NIC discovery remains qualification. |
| Ownership / access | Equivalent ownership; #26 unique exclusive access | Both have process-wide reservations through native teardown. #25 connects `PvAccessControl`; #26 `PvAccessExclusive`. Retain #26; physical exclusion needs final-source qualification. |
| GenICam node/access/errors | #25 unique native tests; #26 potentially deficient | #26 had missing-node smoke checks only. Transfer original XML/IPort fixture and all eight groups against the single `Features` class. Native access queries, exception translation and cache invalidation correct evidenced gaps. |
| Integer ranges/increments | Equivalent basic grid; #25 unique edge coverage | Transfer signed extrema, current bounds/permissions, list-grid refusal and zero-write invalid requests; keep owned results. |
| Float ranges/increments | #25 unique; #26 deficient | `PvGenFloat` lacks increment metadata. Use public `GetNodeMap()` with matching external GenApi headers for fixed/no/list increment modes. No rounding or fabricated grid. |
| String / bool / enum | Equivalent basics; #25 unique tests | Transfer availability, wrong type, RO/WO, missing entry and native query errors; add NUL/empty entry and owned-value tests. No unsupported string/bool setter is added to `IDeviceFeatures`. |
| Shared profile / configuration | Equivalent policy; #25 unique native orchestration tests | Both use unchanged `mono_free_running_device`. Transfer alias selection, dynamic ROI/rate bounds, prevalidation, actual readback, partial-write diagnostics and terminal guards. |
| Mono8 / unpacked Mono12 | Equivalent formats; #26 unique unused-bit check | Preserve pipeline/native-layout/endianness admission and nonzero-unused-bit rejection. Controlled MEL covers both formats. Physical Cognex Mono12 lacks independent endianness evidence. |
| Start / stop / cleanup | Equivalent intent, different architecture | Keep #26 `PvStreamGEV`/`PvPipeline`, bounded polling, uncertain-command retirement and one source/device. Do not import #25 direct QueueBuffer receiver or its retryable pending-state design. |
| Cancellation / restart | Equivalent contract, different pool behavior | Keep deterministic blocked-poll cancellation tests. #25 full-native-pool blocked wait is invalid for copy pools: adapt hardware test to immediate starvation and explicit cancellation. Restart waits for copies to release. |
| Leases / exhaustion / destruction | #26 unique selected architecture | Four host slots independent of eight pipeline buffers, no retained SDK owners. Adapt #25 pressure/release/close/reopen checks to `resource_exhausted`; add destruction with all four copies retained. Stream generations are stream-local. |
| Incomplete / malformed / disconnect | Equivalent rejection; qualification incomplete | Preserve #26 frame validation/release-on-error and sticky failures. Transfer explicit recovery-disable/readback so the SDK cannot silently reconnect. Physical loss, failed native commands and corruption remain pending. |
| Timestamp / MEL | Equivalent invalid timestamps; #25 unique integration contract | Adapt physical contract; add offline SDK-memory variant using production copy stream. Verify zero callbacks/untouched caller bytes without clock, and test-clock-only Mono8/12 delivery, metadata/ID/exact pixels, retained caller buffers, disable/re-enable and teardown. No production clock substitution. |
| Licensing / watermark | #26 unique default-OFF evaluation | Preserve ordinary exclusive Connect, original errors/watermark and prominent diagnostics. #25 rejects `IsLicenseWatermarked()` frames; do not transfer unconditional rejection because it removes selected evaluation capability. Physical MEL contract stays default-licensed-only. Saved evaluation PGM has a warning without changing pixels. |
| SDK isolation / publication | Equivalent isolation; #26 unique safeguards | Preserve SDK-free interfaces, default-OFF/uninstalled targets, scanner, hooks, negative tests and CI. External GenApi headers/runtime remain build-only. No proprietary SDK download in CMake/hosted CI. |
| Provision / launch / version | #25 unique | Transfer pinned external-only provisioning, checksum-verified header extraction and process launcher. Add SDK-free safety tests, tested-header/runtime checks. Keep `GIGE_MEL_PLEORA_SDK_ROOT`. |
| Diagnostics / host procedure | #25 unique useful options | Transfer help/version, current-only capture (`capture-current`), explicit `apply`, and adapted Debian 13 task. Existing `capture` retains documented volatile full-frame/10-Hz behavior. Duplicate inspection API is not imported; canonical diagnostic reports native identity/profile failures. |
| Physical evidence | #26 unique historical acquisition | Retain Cognex Mono8 watermarked evaluation record/limitations. #25 runtime probes did not qualify Ethernet reception. Neither qualifies final source, unrestricted pixels, production MEL time or entitlement. |

## Engineering scope and remaining gates

GIGE-MEL-028 independently isolates the native GenApi TSan inversion to the
external log4cpp runtime's initialization/global-cleanup lock order. See the
[reproducer, exact lock stacks and blocked disposition](genapi-tsan-lock-order.md).
No suppression is approved or added; combined TSan qualification remains blocked.

No backend interface, MEL declaration, factory signature, shared profile source,
production stream implementation or published history is replaced. Native GenApi
validation helpers from #25 were adapted into the existing `Features` class for
actual increment/cache/error facts. Exactly one backend/factory and pipeline/copy
stream remain. Test sources are not alternative transports. The historical
`aravis_virtual_clock` helper is SDK-free and linked only into tests.

External GenApi ZIP/member checksums are verified by the transferred helper;
headers stay outside the repository. Mirror availability or a working runtime
does not establish Pleora development entitlement. No license manipulation or
watermark removal is performed.

See [integration](pleora-integration.md#consolidated-native-contracts-gige-mel-027)
and the [Debian 13 task](tasks/GIGE-MEL-027-pleora-debian13.md). Final exact commit/
tree and executed checks are recorded in PR #26 after commit and HEAD reruns;
a commit cannot contain its own hash.

Licensed physical Mono8/native+MEL, compatible authorized Mono12, disconnect/
uncertain-command paths, unrestricted sensor fidelity, timestamp/optics,
developer/receiver entitlement and vendor Debian support remain unestablished
by offline tests. Both PRs remain open if required applicable checks fail or
consolidation is incomplete. Production readiness is not claimed even if the
duplicate can later be closed as superseded.