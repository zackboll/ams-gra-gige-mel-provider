# Optional Pleora eBUS backend — GIGE-MEL-025

**State: IMPLEMENTED / PENDING_VENDOR_LICENSE_AND_HARDWARE_QUALIFICATION.**
Not `READY_FOR_INDEPENDENT_REVIEW`. Native control/acquisition code has compiled,
but real device open is blocked by SDK licensing. No physical Pleora frames or
performance results are claimed. No new MEL capability, provider factory
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
followed by the adapter's license check reports **“Unlicensed. Watermark, limited
use”**, so open returns `unsupported` before Connect. Obtain applicable vendor
rights and a legitimately licensed setup before physical acquisition. User
download authorization does not establish Pleora licensing rights. No license
enforcement is bypassed and no SDK material is redistributed by this project.

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

## Build and run (licensed external SDK only)

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
licensing, then uses `PvDeviceGEV` exclusive access plus a process-wide ownership
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

## Qualification record (2026-11-12)

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
