# Optional Pleora eBUS backend — GIGE-MEL-025

**State: watermarked receiver hardware evaluation (GIGE-MEL-025-R1).**
Normal SDK Connect and real Mono8 image reception succeed with explicit
evaluation selected. SDK development entitlement and production licensing remain
unresolved. This is not production qualification. No new MEL capability, provider factory
selection, public ABI, navigation transport or clock mapping is added.

## External SDK, provenance and license

On Debian 13.7 x86-64, no usable SDK was found in `/opt`, `/usr/local`,
`/home/zboll/sdk`, the environment or the `pleora-navigation-receiver` project.
The latter contained its own test stubs and an external-include recipe.
The user-authorized third-party mirror was retrieved with Git sparse checkout:

- Repository: <https://github.com/RIVeR-Lab/ibex>
- Detached commit: `fa8139f75362eb377e97354f1eb079f042076b1a`
- Sparse directory: `docs/place_in_opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64/`
- Local checkout: `/home/zboll/sdk/pleora/ibex-mirror`
- SDK root: `/home/zboll/sdk/pleora/ebus_sdk`, a symlink to that external directory.

No SDK files entered the project working tree. Original directory/library
symlinks, EULA and third-party notices remain intact. Git fsck completed without
reported errors; checkout status was clean. An external SHA-256 inventory of
1,464 regular files is retained at `/home/zboll/sdk/pleora/sdk-sha256.json`.
These checks establish consistency with this mirror, **not official vendor
authenticity, redistribution rights, or entitlement**. No SDK installer, driver
script, system tuning script, sample executable or binary patch was used.

The retained EULA is under the SDK's `licenses/EULA/EULA.txt`. It ties a license
grant to purchasing a Pleora solution, restricts copying and independent
redistribution, and prohibits reverse compilation/disassembly. Vendor entitlement
for this workstation and Cognex combination has not been established. Discovery
reports **“Unlicensed. Watermark, limited use”** (no trailing period in this SDK).
Default open still returns `unsupported` before Connect. Explicit receiver
evaluation can delegate permission to the SDK as described below; it does not
establish development rights. User download authorization does not establish
Pleora licensing rights. No license
enforcement is bypassed and no SDK material is redistributed by this project.

### Explicit watermarked receiver evaluation

Sources consulted on 2026-11-12:

- [Pleora eBUS Receive](https://www.pleora.com/machine-vision-connectivity/ebus-receive/)
  states that activating a receiver license removes the embossed watermark on
  received images, and separately requires a Developer Seat license for SDK access.
- [Pleora FAQ](https://www.pleora.com/resources/frequently-asked-questions/)
  describes free Player Toolkit evaluation with watermarked images, but says
  there is no SDK Seat License trial. This is not independent evidence of a
  development entitlement for this adapter or workstation.
- Installed `share/doc/sdk/class_pv_device_info.html` describes
  `IsLicenseValid()` as a license-validity query and says third-party Connect
  needs a license. The installed `class_pv_device_g_e_v.html` lists `NO_LICENSE`
  among Connect results. These references do not promise SDK evaluation access.

The internal `Options::allow_watermarked_evaluation` is **false by default**.
When explicitly true, open records the unchanged validity/message and attempts
exactly one ordinary `PvDeviceGEV::Connect(selected, PvAccessExclusive)`. SDK
failures retain their name, numeric code and description. There is no fallback,
adapter retry, state override, license-file modification or watermark handling.
Diagnostics prominently label unlicensed/watermarked evaluation and unresolved
development entitlement; without a diagnostic callback they go to `std::clog`.
The observed SDK permission is technical behavior, not legal entitlement or an
unrestricted receiver license. Never advertise evaluation as production-qualified.

### Versions and Debian compatibility

Do not describe every component as 7.5.3:

| Component | Observed version |
| --- | --- |
| `EbVersion.h`, transport and utility libraries | 7.5.3.7155 |
| `PvVersion.h`, `libPvDevice`, `libPvStream`, other `libPv*` files | 6.5.3.7155 |
| Pv shared-library SONAMEs | `libPv*.so.6.5` |
| Embedded GenICam library names | `gcc48_v3_4`, Linux64_x64 |

ELF metadata identifies x86-64, little-endian, dynamically linked Linux libraries.
The device library references GLIBCXX through 3.4.21; the base library references
glibc through 2.34. With both external library directories on `LD_LIBRARY_PATH`,
`ldd` resolves device/stream dependencies on Debian 13; none was reported missing.
GCC 14.2 builds and links the original adapter with warnings as errors. Runtime
discovery and memory-buffer tests exercise loaded C++ symbols. This is not a
vendor-supported Debian claim or full ABI certification. Vendor binaries are
not sanitizer-instrumented.

## Build and run (external SDK; entitlement remains the operator's responsibility)

The default build does not search for or link Pleora. The optional target,
diagnostic and private headers are never installed/exported. SDK roots inside
the project are rejected. Configure/build does not download anything:

```sh
cmake -S . -B build/pleora -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DGIGE_MEL_BUILD_PLEORA=ON \
  -DGIGE_MEL_PLEORA_SDK_ROOT="$HOME/sdk/pleora/ebus_sdk"
cmake --build build/pleora --parallel 2
ctest --test-dir build/pleora --output-on-failure

# Set explicitly; do not execute mirrored setup/install scripts.
export PUREGEV_ROOT="$HOME/sdk/pleora/ebus_sdk"
export GENICAM_ROOT_V3_4="$PUREGEV_ROOT/lib/genicam"
export GENICAM_CACHE_V3_4="/tmp/gige-mel-genicam-cache"
export LD_LIBRARY_PATH="$PUREGEV_ROOT/lib:$GENICAM_ROOT_V3_4/bin/Linux64_x64"
build/pleora/gige_mel_pleora_diagnostic list
build/pleora/gige_mel_pleora_diagnostic inspect EXACT_DISCOVERED_MAC
# Active, volatile full-resolution 10 Hz test; restores original ROI/timing.
# Only after entitlement, camera/network setup and exclusive access are verified:
build/pleora/gige_mel_pleora_diagnostic capture EXACT_DISCOVERED_MAC
# Explicit, unqualified receiver evaluation; SDK remains authoritative:
build/pleora/gige_mel_pleora_diagnostic --allow-watermarked-evaluation inspect EXACT_DISCOVERED_MAC
# New private output path; saves every Mono8 row including the watermark:
build/pleora/gige_mel_pleora_diagnostic --allow-watermarked-evaluation \
  capture EXACT_DISCOVERED_MAC 20 /absolute/private/path/sample.pgm
```

CTest sets its external runtime paths and GenICam root/cache. Omitting
`GENICAM_ROOT_V3_4` causes the SDK to exit during initialization; supply it before
launching an SDK-linked process. No SDK initialization or environment mutation
is performed by the factory. Keep local build trees and their SDK-dependent
executables private; **never upload them as CI artifacts**. Hosted CI runs the
SDK-free guard/tests only; it does not download Pleora.

## Implemented source contract

`gige_mel::backend::pleora::make_backend()` returns `ICameraBackend` without I/O.
Discovery uses `PvSystem`, filters `PvDeviceInfoGEV`, deduplicates by lowercase
MAC, and returns stable MAC IDs. Open rediscovers that exact MAC and checks
licensing (with the explicit evaluation policy above), then uses `PvDeviceGEV` exclusive access plus a process-wide ownership
reservation retained until native disconnect/heartbeat destruction. It never
falls back to another device, read-only access or an IP/display-name alias.
The link-disconnected sink latches terminal loss; it does not call users or
automatically reconnect. Discovery uses a 1 s detection timeout; control calls
use SDK transaction/retry defaults (not a hard whole-operation deadline).

`Features` implements the **existing** `IDeviceFeatures`, checking type, access,
implementation and availability through checked result-returning SDK queries.
Current integer bounds/increments and float bounds are checked at every write.
`PvGenFloat` exposes no increment query: snapshots report no advertised increment,
not an invented grid. Native units are read, not converted by the adapter.
Ordinary SDK failures retain native code/name/text, including `NO_LICENSE`.

The unchanged `mono_free_running` module supplies aliases, preconditions,
capabilities, integer-Hz/nanosecond conversions, complete-request validation,
write order, partial-failure diagnostics, readback, acquisition preflight,
pool arithmetic and frame-layout validation. No camera policy is duplicated.
Configuration and apply require stopped acquisition. Open does not silently fix
trigger, auto-exposure, fractional rate, disabled rate control or pixel format.
Mono8 is supported; unpacked Mono12 is admitted only with the shared profile's
independent `DeviceStreamChannelEndianness=Little` readback. Packed conversion
and SDK watermark removal are not implemented.

Acquisition requires current `AcquisitionMode=Continuous` without changing it.
It reads `PayloadSize` (not the SDK fallback-size helper), sets a volatile stream
destination, creates `PvStreamGEV`/`PvPipeline`, disables automatic buffer growth,
starts the pipeline, enables transport and executes AcquisitionStart. Eight
native buffers and four host-copy slots are admitted separately within
64 MiB/frame and 256 MiB/pool budgets (512 MiB combined maximum plus SDK overhead).
The copy path owns no SDK object in a `FrameLease`. Every retrieved buffer is
returned after validation/copy, including errors. A private native-source seam
permits controlled tests without replacing profile policy.

Receive polls at most 10 ms per SDK call, bounded by the caller's timeout
(0..24 hours), with atomic cancellation checked between polls. Stream/device
locks pin the SDK graph through retrieval, validation, copy and release. Logical
close drains the in-flight slice/copy before native teardown; receive then
returns stopped. Callers must join receive before destroying the stream. No
consumer callback runs in this adapter. Stop cancels before waiting for the stream
boundary. Statistics are synchronized; incomplete frames are discarded/counted.
Unsupported layouts/chunks/payloads and inconsistent sizes fail explicitly.
Mono12 high unused sample bits are checked. Production timestamps remain
`FrameTimestamp{}`: the existing MEL provider consequently rejects these frames.

Retained copies stay immutable after stop, close and object destruction. A full
pool returns `resource_exhausted`. Restart refuses while previous leases are
retained, bounding memory across runs. Successful restart advances generation.
Stop is attempted even after an uncertain start acknowledgement; cleanup or
native-buffer-release failure retires the session instead of silently restarting.
Persistent user sets, IP settings, packet spacing, packet-size negotiation,
UTC/PTP and navigation transmission are untouched.

## Original qualification record (before R1, 2026-11-12)

- Initial ping: no reply from `192.168.24.101`; route used `192.168.9.1` from
  `enp5s0` at `192.168.9.10/24`, not recorded `192.168.24.10/24`.
- Pleora broadcast discovery **found** the CIC-5000R at the recorded IP.
  A concurrently running Aravis test simulator was also observed; it is not
  physical-camera evidence. No two backends controlled the Cognex simultaneously.
- Cognex open: **blocked by license check**, “Unlicensed. Watermark, limited use”.
  No camera configuration write or Pleora acquisition was attempted.
- Network configuration stayed unchanged; noninteractive sudo was unavailable.
- Real camera readback, 10 Hz full-resolution Mono8 integrity, Mono12, timeout,
  cancellation, ownership, restart and shutdown are **not qualified**.
- Same-run physical Aravis/Pleora throughput, latency, drops and behavior
  comparisons are **not available**. Earlier Aravis results in
  [the camera record](cognex-cic-5000r.md) are historical, not a paired benchmark.

`pleora_adapter_contract` uses actual SDK-allocated buffers with an original
controlled source, not network reception or a licensed device. It checks copy
isolation, full pools, retained frames after destruction, restart generations and
refusal, bounded timeout/cancel, malformed/incomplete images, Mono12 samples,
and release/stop failures. It does not qualify native Connect, feature setters,
failed acquisition commands or physical disconnect. Those paths need a licensed
SDK/camera setup and further fault tests.

Executed local checks (GCC 14.2, Debian 13.7): default development CTest 12/12;
Pleora-enabled CTest 13/13; combined Aravis 0.8.36 + Pleora CTest 20/20 including
Aravis Fake and GigE loopback; Pleora-enabled ASan/UBSan 13/13 and TSan 13/13.
Clang also built the optional adapter and passed 13/13. The sanitizer runs
instrument project code, not proprietary libraries. The default and optional
builds were installed to separate temporary prefixes; both separate installed
consumers passed. Installed non-documentation names/text and provider `ldd`
contained no Pleora dependencies/references. Upstream integrity verification and
`git diff --check` passed. Hosted CI is configured, not claimed executed here.

## R1 watermarked hardware qualification (2026-11-12)

**Disposition: `WATERMARKED_HARDWARE_QUALIFIED` for the tested Mono8 receiver
evaluation and lifecycle scope only. Not production-qualified, not licensed
operation, not full MEL conformance.** The previous record above is historical.

Tested Debian 13 x86-64, the external SDK/version combination in the table above,
and Cognex CIC-5000R, exact discovered MAC `00:30:53:1c:9a:65`, serial/DeviceID
`21874533`, firmware `106349-08;U;CIC-5000R;V3.7-1;0`. Native identity queries
returned `COGNEX`, `CIC-5000R`, `Mono8`, and `Continuous`.

The loaded device/stream/buffer/GenICam/system/base libraries were
`libPv{Device,Stream,Buffer,GenICam,System,Base}.so.6.5.3.7155` from the external
SDK `lib` directory. Transitive transport/utilities were
`libEbTransportLayerLib.so.7.5.3.7155` and `libEbUtilsLib.so.7.5.3.7155`;
GenICam resolved `libGCBase_gcc48_v3_4.so` and `libGenApi_gcc48_v3_4.so` from
`lib/genicam/bin/Linux64_x64`. An external `ldd` record and library SHA-256
inventory retain the exact resolved paths; no missing libraries were reported.

### Network, license and profile admission

- Initial host: only `192.168.9.10/24` on `enp5s0`, camera route via
  `192.168.9.1`. Three duplicate-address probes for `192.168.24.10` received no
  response. The operator temporarily added `192.168.24.10/24`; the original
  address and default route remained. Three camera pings succeeded and discovery
  found exactly one physical GigE camera. No persistent network/camera-IP change.
- Default inspect rejected before Connect, as before. Explicit evaluation
  attempted normal exclusive Connect and the actual SDK result was **OK**.
  The SDK discovery object's license status remained **false**, with unchanged
  **`Unlicensed. Watermark, limited use`**, before and after Connect.
  No actual `NO_LICENSE` occurred on this camera; denial is covered by controlled
  tests and preserves native code/name/description without retry.
- The initial connected camera was correctly rejected by shared-profile
  admission because `AcquisitionFrameRateEnable=false`. The existing external
  helper temporarily set only volatile rate enable=true and rate=10 Hz, with
  readback. The shared profile was not modified. Open/configuration/close then
  succeeded, at full-frame 2592×1944 Mono8, 10 Hz, 35 ms. Both initial and extended
  sessions restored enable=false/rate=10, and verified all recorded writable
  features against their pre-session snapshots. No UserSet/IP/firmware writes,
  packet-spacing change, watermark manipulation or kernel tuning occurred.
- After testing, noninteractive sudo still could not remove the temporary host
  address. The operator explicitly requested it remain for their **ongoing camera
  test session**. Cleanup is handed off: when that session ends, remove only
  `192.168.24.10/24` from `enp5s0` with `sudo ip addr del 192.168.24.10/24 dev enp5s0`.
  The task leaves no camera controller running.

### Initial images and extended acquisition

The diagnostic captured **20/20 complete frames**, IDs **1–20**, with zero
reported incomplete frames, receive timeouts or host-pool starvation. Every
frame was 2592×1944 Mono8, 2592-byte row stride, 5,038,848 bytes. A full,
unmodified PGM and lossless PNG were saved outside the repository. Visual
inspection confirmed the **Pleora Technologies / eBUS SDK / FOR EVALUATION**
watermark. The dark sample had nonzero payload values (range 0–58). This is
evidence of received data, **not exact sensor pixel fidelity**. No evaluation
dimension/format/stride adjustment or acceptance relaxation was needed.

A private original harness drove both production adapters sequentially, never
controlling the camera with both at once. Each sustained run measured about
60 seconds at the same full-resolution 10 Hz/35 ms settings; descriptors,
complete status, byte span and invalid timestamps were checked on each delivered
frame. The stream discards/counts incomplete SDK buffers rather than publishing
them as complete. The comparison was one run per adapter, not a controlled
performance benchmark; compilation/regression activity varied host load.

| Metric | Aravis 0.8.36 | Pleora watermarked evaluation |
| --- | --- | --- |
| Discovery (single measurement) | 1.00180 s | 1.01355 s |
| Exact-device open (single measurement) | success, 1.12202 s | success, 1.23496 s |
| Acquisition wall duration | 60.0052 s | 60.0029 s |
| Complete delivered frames | 595 | 598 |
| Delivered rate, first-to-last span | 9.91637 Hz | 9.96643 Hz |
| Incomplete frames discarded | 5 | 2 |
| Receive deadline timeouts (250 ms) | 1 | 0 |
| Delivered frame-ID gap events | 4 | 2 |
| Host-pool starvation during sustained run | 0 | 0 |
| Process CPU time during sustained run | 5.48710 s (~9.14% of one CPU) | 4.86235 s (~8.10% of one CPU) |
| Process peak RSS observed at end of run | 54,144 KiB | 102,600 KiB |
| Three waiting-receive cancellation/stop cycles | 99.34–99.38 ms | 144.35–145.03 ms |
| Stop/restart, close/reopen, retained bytes | passed | passed |
| Watermark | not applied | SDK watermark present, intact |

CPU time is process user+system time over acquisition; RSS is process high-water
memory, not an isolated stream allocation measurement. Aravis warned that the
4 MiB receive-buffer request was capped at **212,992 bytes** by current host
`net.core.rmem_max`. The host setting was left unchanged. Neither delivered
counts nor frame-ID gaps establish packet-loss-free performance or packet-loss
counts. There were incomplete frames on **both** adapters; no zero-loss claim.

The physical harness additionally passed:

- Process ownership exclusion while open; normal close and subsequent reopen.
- Three start/stop cycles, a bounded zero-duration receive timeout, and cancellation
  of a receive confirmed still waiting (10 s requested) after 5 ms, with stop/join
  under 500 ms; next start received another complete frame.
- Retaining four leases exhausted the host pool: Pleora returned
  `resource_exhausted`, Aravis timed out with its native four-buffer pool held.
  Pleora refused restart with outstanding copies, as designed. Native buffers
  remained recyclable independently of retained Pleora copies.
- Byte-for-byte retention of the **already-watermarked** first retained copy
  across stream teardown, device close and object destruction; explicit lease
  destruction then successful reopen/start/receive/stop/close. This compares
  retained storage only, not Pleora pixels with Aravis sensor data.

A supplemental sequential run repeated the lifecycle checks with a **1 ms**
receive deadline (each returned timeout within the 100 ms test bound), rather
than just zero-duration polling. It again delivered 20 complete frames per
adapter at approximately 10.0004 Hz with no incomplete frames/timeouts during
the two-second acquisition phase. Deliberate four-lease pressure recorded one
starvation event on each adapter; cumulative timeouts including the three
intentional short-deadline tests were 3 for Pleora and 4 for Aravis (the latter
includes the deliberate full-pool timeout). These counters are separate from
the 60-second table. Cancellation measured 143.03–143.43 ms for Pleora and
98.23–98.31 ms for Aravis. A final independent feature snapshot verified every
recorded writable value restored, even after the outer tool's 30 s reporting
deadline expired during the supplemental script's final snapshot.

No unexpected acquisition termination occurred. Physical unplug/reconnect was
not performed (no safe remotely controlled physical-disconnect facility); real
link-loss recovery remains unqualified. Mono12 was **not selected**: feature
inspection found `DeviceStreamChannelEndianness` absent, so required independent
Little-endian admission was unavailable despite format enumeration. All camera
timestamps remain invalid/unqualified. Development entitlement, unrestricted
receiver licensing, Debian vendor support, sensor fidelity and production
qualification remain outstanding.

Raw captures, private harness/helper use, logs, pre/post state, and library
inventories are retained only at `/home/zboll/hardware-evidence/gige-mel-025-r1`,
outside the working tree; they are not PR artifacts.

### R1 regression and isolation checks

Executed locally with GCC 14.2 (vendor binaries are not instrumented):

| Configuration | CTest result |
| --- | --- |
| Default development, no camera SDK required | 12/12 |
| Pleora enabled | 13/13 |
| Combined Aravis + Pleora | 20/20 |
| Default ASan/UBSan | 12/12 |
| Default TSan | 12/12 |
| Pleora ASan/UBSan | 13/13 |
| Pleora TSan | 13/13 |
| Combined ASan/UBSan | 20/20 (exclusive rerun) |
| Combined TSan | 20/20 |

The final license-test assertion/text update was rebuilt and its contract test
rerun in all optional configurations. The original Aravis-only sanitizer build
directories failed regeneration because their cached SDK paths were stale;
fresh combined sanitizer builds used the installed Aravis 0.8.36 pkg-config
directory. The first combined ASan run failed its loopback fixture because
the concurrent TSan suite owned UDP 3956; after TSan finished, the **entire**
ASan suite passed alone. No sanitizer finding was suppressed or attributed to
these environment failures. Existing Aravis Fake, Mono12 simulated, timestamp
and GigE loopback tests pass in the combined suites. Hosted CI execution is
not implied, and no proprietary SDK download/artifact job was added.

The new controlled license-policy tests cover the unchanged default rejection,
explicit single Connect delegation, immutable observed status, prominent
unlicensed diagnostics and exact native `NO_LICENSE` denial propagation. Existing
buffer-release, cancellation, stop-failure, pool and retention contracts pass;
real ownership/cleanup/reopen is also covered by the private hardware harness.

Upstream 97-file and JSON integrity checks pass. All **1,464** external SDK files
still match the prior private SHA-256 inventory. A combined installation and
separate installed consumer pass; the package-isolation verifier passes for
Aravis, and installed public text/symbols/provider dependencies contain no Pleora
SDK dependency. Debug binaries retain their build-directory name
(`build/pleora-aravis`) in debug information, not an SDK reference. Publication
uses the existing staged/history proprietary-content guard and hooks; no SDK,
capture, credential or raw hardware evidence is staged/published.

## Publication safeguards

Enable local hooks with `git config core.hooksPath .githooks`.
`scripts/check_proprietary.py` reads **Git blobs**, not working-tree substitutes.
It rejects recognizable vendor filenames/declarations/copyright/EULA, ELF/archive
magic, binary/installer/archive/license-key extensions, private-key/token patterns,
SDK directories, symlinks and submodules. `--history BASE` also scans all new
commit trees, including deleted intermediate files. Negative fixtures test staged
content and deleted-history detection. Pre-commit, pre-push and hosted CI invoke
it; `.gitignore` is additional protection, not the enforcement boundary.

Before publication manually inspect the staged diff, run the guard on index and
branch history, and compare proposed blobs with the external SDK inventory.
Pattern checks are defense in depth, not proof against all altered vendor content.
No SDK header/source/library/sample/license/key, SDK-dependent executable or
private evidence folder belongs in this repository or its hosted artifacts.
