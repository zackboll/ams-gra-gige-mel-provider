# GIGE-MEL-GENTL-003 — Qualification evidence

Companion [design](GIGE-MEL-GENTL-003-design.md). Incorporated `main`:
`9ded1b496c7e3b3811f78d57d3681c817256c555` (PR #42). Final commit/tree and
hosted status are reported on the PR to avoid self-reference here.

## 1. Disposition and evidence classes

**Physical qualification: `BLOCKED_BY_PRODUCER_BEHAVIOR`.** Balluff crashes
during offline interface update; no workaround or subsequent camera stage was
attempted. Preferred Euresys additionally has `VENDOR_AUTHENTICATION_REQUIRED`
(`BLOCKED_BY_PRODUCER_ACCESS`). Physical authorization has **not been granted**
for either producer. Tooling/offline review readiness is separate from these
unclosed qualification gates.

| Evidence class | Executed in GENTL-003 | What it establishes |
| --- | --- | --- |
| Simulated project fixture | Yes, local contract matrix below | Consumer/backend guards and existing profile/lifetime behavior, not vendor compatibility |
| Real producer, no camera | Balluff Impact Acquire 3.7.2.4495, GenTL 1.6 | Three successful load/close children; TLOpen success, interface-update SIGSEGV |
| Real producer, simulated camera | **Not run** | Read-only wire behavior, ownership release and post-close quietness unknown |
| Physical Cognex discovery | **Not run** | Historical identity/NIC/address not revalidated |
| Physical Cognex read-only open/inspection | **Not run** | GenApi, profile admission and configuration readback unknown |

No image acquisition, camera-settings write or NavigationReport transmission.
No physical NIC changes, elevated SDK installation or system-library changes.

## 2. Environment and evidence storage

Executed 2026-11-18 UTC (host local date 2026-11-17), Debian 13 x86-64,
Linux `6.12.116+deb13-amd64`, glibc 2.41, Python 3.13.5. SDK and original
downloads remain under `/home/zboll/sdk/gentl-001/`. Task logs, supervisor
manifest, exports and dependency results are under
`/home/zboll/gentl-003-evidence/` (external, not versioned). The qualification
runner is `/home/zboll/gentl-003-evidence/qualify.sh`.

Aravis GenApi dependency for optional builds:
`/home/zboll/sdk/aravis-0.8.36/lib/pkgconfig/aravis-0.8.pc`, 0.8.36,
provisioned by the existing checksum-verified script. GenTL header:
`/home/zboll/sdk/gentl-001/header/include/GenTL.h`, official EMVA 1.6 SHA-256
`5b656a413f4cdc347e80494b87f94b09ac925e391a9022edb7f028d996e35e4e`.
No proprietary headers or SDK files were added to Git.

## 3. Euresys investigation — not obtained, not tested

Official source: <https://www.euresys.com/en/download-area/>. The page retrieved
through the documentation fetch service lists eGrabber **26.10**,
`egrabber-linux-x86_64-26.10.0.11.tar.gz` (383.98 MB), release notes
`egrabber-release-notes-26.10.0.2224.pdf`, and offline documentation
`egrabber-linux-offline-documentation-en-26.10.0.2224.tar.gz`.

GENTL-001 already recorded the SDK CMS API responding **HTTP 403**,
`File is protected, please authenticate to download.` That protected URL was
**not retried**. A single direct default Python urllib request to the public
download-area page in this task also returned HTTP 403; no authentication
bypass, URL guessing or repeated request followed. The fetch-service page
listing is not evidence that the SDK itself can be downloaded anonymously.
The local `euresys-access.json` preserves these distinctions. No authenticated
Euresys session or SDK archive was available. Interactive vendor login and an
owner-supplied authenticated download are external prerequisites.

The existing official 26.10 release notes were read externally (SHA-256
`d42acdf23dd40faa5b632c9cf49e44a6ab9064fdc946782d87e7f832fb00f636`).
Findings from documentation, **not a binary inspection**:

| Question | Finding / limit |
| --- | --- |
| Gigelink inclusion | Release notes pp. 20–22 describe `gigelink.cti` among four delivered producers; downloaded package contents cannot be verified without the archive |
| Third-party hardware | Gigelink described as a free-of-charge, hardware-independent GigE Vision camera library, including GigE Vision 3.0 RDMA cameras; no Euresys acquisition hardware requirement is stated for that library |
| Exact GenTL version/exports | **Unknown**; historical header notes are not a runtime version query or export table |
| Runtime dependencies | Notes specify glibc ≥2.23 and eGrabber C++ GLIBCXX_3.4.21/CXXABI_1.3.9; actual Gigelink `DT_NEEDED`, paths and dynamic initialization dependencies **not measured** |
| Linux initialization | Notes p. 25 say install Memento before eGrabber; separate NVIDIA RDMA instructions exist. Gigelink-only minimum/extraction initialization remains unverified |
| Root/system modifications | Full installation has system/driver implications; whether externally extracted Gigelink alone works without root **unknown**. No installer executed |

Next step: obtain the archive through the vendor's authenticated process,
retain it externally, inspect archive manifests/scripts/exports/dependencies
before executing anything, and test extracted runtime only if supported. Do not
claim a tested Euresys version or camera combination.

## 4. Balluff — actual offline producer execution

Existing extracted Impact Acquire 3.7.2 runtime (installer not run):
`/home/zboll/sdk/gentl-001/balluff/x/ImpactAcquire-x86_64-3.7.2` (`ROOT` below).
Native producer `lib/x86_64/libmvGenTLProducer.so.3.7.2`, SHA-256
`247d68e2ab1be3155889029cc361e8688b2084de03d3fde0b52f967d884ee9e4`.
External explicit `.cti` symlink:
`/home/zboll/gentl-003-evidence/balluff.cti` → that exact library.

`balluff-exports.txt` retains `nm -D --defined-only`. The project-owned consumer
resolved every mandatory entry and its optional write/acquisition groups (no
optional absence reported). Actual producer identity:

* Vendor `Balluff`, version `3.7.2.4495`, GenTL **1.6**, type `Mixed`.
* ID `{AF542A5A-E6D3-4f3d-9908-4A89AE21105A}`.
* Model reports GigE Vision (Filter driver and Socket) 2.1 and USB3 Vision 1.0.1.

Static `DT_NEEDED` and `ldd` evaluated: `libdl.so.2`, `librt.so.1`,
`libpthread.so.0`, `libexpat.so.1`, `libstdc++.so.6`, `libm.so.6`,
`libgcc_s.so.1`, `libc.so.6`, `ld-linux-x86-64.so.2`; resolved system libraries
under `/lib/x86_64-linux-gnu/` and `/lib64/`. Embedded RPATH points at a vendor
build-agent expat directory; no host library was installed there. Runtime
environment also supplied external `$ROOT/lib/x86_64` and
`$ROOT/runtime/bin/Linux64_x64` (GenApi/GCBase 3.5); this does not imply these
are all lazily loaded dependencies of unexecuted device operations.

Reproducible executed command, from `/home/zboll/git/ams-gra-gige-mel2`:

```sh
ROOT=/home/zboll/sdk/gentl-001/balluff/x/ImpactAcquire-x86_64-3.7.2
env MVIMPACT_ACQUIRE_DIR="$ROOT" GENICAM_ROOT="$ROOT/runtime" \
  GENICAM_ROOT_V3_5="$ROOT/runtime" \
  GENICAM_CACHE_V3_5=/home/zboll/gentl-003-evidence/genicam-cache \
  LD_LIBRARY_PATH="$ROOT/lib/x86_64:$ROOT/runtime/bin/Linux64_x64" \
  python3 /home/zboll/git/ams-gra-gige-mel2/scripts/gentl_qualification.py \
  --probe /home/zboll/git/ams-gra-gige-mel2/build/gentl-backend/gige_mel_gentl_probe \
  --producer /home/zboll/gentl-003-evidence/balluff.cti \
  --output /home/zboll/gentl-003-evidence/balluff-offline --timeout 20 --repeat 3
```

Output directory must be new; do not rerun a crashed sequence automatically.
The producer run used the working-tree probe built before the final additional
fixture-only failure-path cleanup regression; its SHA-256 is recorded in the
bundle. At execution time children inherited the checkout working directory.
The final supervisor sets their working directory to the external output folder
to keep potential future producer artifacts/core dumps outside Git. The actual
crash was **not rerun** after these tooling changes; the immutable earlier logs
are the real-producer evidence, not an assertion of final-binary requalification.
Actual supervisor exit **1**. Its `result.json` and individual logs record:

| Operation/stage | Actual result |
| --- | --- |
| Namespace availability | exit 0 |
| `load-1`, `load-2`, `load-3` (separate children) | exit 0 each, ~2.27 s each; load/function table/GCInitLib, GCGetInfo and GCCloseLib all reported successful |
| `interfaces`: load/init/info | succeeded |
| `TLOpen` | succeeded; subsequent `TLGetInfo` printed actual system identity |
| `TLUpdateInterfaceList` | **SIGSEGV**, returncode **-11**, signal **11**, no timeout (child 2.618390 s; exact time in JSON) |
| Interface IDs/info, `TLClose`, subsequent `GCCloseLib` | **Not reached** in the crashed child |
| Discovery/device open/port reads | **Not requested / not run** |

Last line: `BEGIN TLUpdateInterfaceList`. No `END TLUpdateInterfaceList`.
No useful native error return is possible from this crash. No speculative
workaround applied; qualification terminated. No stack trace available: core
limit was `0`, no core was produced, and the crash was not rerun under a debugger.
This reproduces the prior GENTL-001 operation-level failure with loopback down;
it does not prove its internal root cause. The isolated namespace had no host
NIC and its loopback was left down. No packet capture was taken: **no wire
read-only/heartbeat evidence is claimed**.

### Producer-local GigE configuration and licensing

The extracted SDK's `ignoredInterfaces.txt` documents interface-type/ID `I`
(ignore) and `U` (unignore); no valid entries means ignore nothing. Its current
contents include **`GEV=I` and `U3V=I`**. Current bytes were hashed externally
and not changed. This may differ from earlier temporary experimental settings;
the earlier observation of five U3V interfaces is historical, not this run.
Removing `GEV=I` is the documented local unignore mechanism, but the file alone
does not establish safe networking, exact file lookup under relocation, read-only
behavior or permission to use third-party hardware. It was **not enabled or
tested**, particularly after the crash. No global interface changes are needed
to edit an external producer configuration; no safe operational recommendation
is made from that fact.

SDK `doc/EULA.txt` §§3.1–3.3 requires an express individual grant for paid or
gratuitous third-party hardware use. No such grant was available. Cognex
contact through Balluff is blocked independently by vendor conditions, the
crash, unverified read-only behavior and missing physical authorization.

## 5. Cognex / XML / byte-order comparison — not executed

The historical Aravis baseline is **not substituted** for GenTL observations:

| Property | Historical baseline | This qualification |
| --- | --- | --- |
| Identity | COGNEX / CIC-5000R / `106349-08;U;CIC-5000R;V3.7-1;0`, serial 21874533 | Not read |
| Address/interface | Historical `192.168.24.101`, `enp5s0` | Not actively checked; no discovery authorization stage reached |
| XML | 1,108,591 bytes, SHA-256 `15bee3f741af8ee2582a88b73fe62515971eeb1db9f7e7f73e6ea64192f8d73f`, legacy schema 1.0.1 | No vendor-retrieved URL, bytes, size, SHA-1/SHA-256 or schema |
| Port byte order | XML uses big-endian registers; producer declaration must be measured separately | Unknown; no register bytes/value conversion comparison |
| Sensor geometry | 2592×1944 | Not read |
| Pixel format | Historical Mono8 | Current state unknown |
| Rate enable | Boolean, power-on false | Not read; profile still requires true |
| Frame rate | `AcquisitionFrameRateAbs`, Hz | Not read |
| Exposure | `ExposureTimeAbs`, µs | Not read |
| Trigger/acquisition | Historical Off / Continuous | Not read |
| Device close | Earlier SDK evidence only | No GenTL DevClose or heartbeat/ownership result |

Real producer XML form (uncompressed Local/ZIP/File/http), metadata consistency
and SHA-1 presence are unknown. Existing unsupported URL/ZIP errors, size bounds,
well-formedness and hash validation remain unchanged and fixture-tested. No ZIP
reader added; see the design for a separate security-scoped recommendation if
later evidence establishes a need. No profile admission or configuration
readback attempted, and no settings changed to force admission.

## 6. Ownership and test qualification

Real load-only children reported successful GCCloseLib and normal process exit;
that is not independent proof of vendor-internal resources or actual unmap.
No native resource chain successfully closed in the interface child: its process
crashed. No real device existed, so device close order, unload while a device
lives, reopening after cleanup, heartbeat cessation and native failed-close
behavior are **unestablished**. No destructive native failure injection or
connection-loss experiment was performed on hardware.

Existing GENTL-001 consumer and GENTL-002 backend fixture tests remain the
evidence for no double-close, child-parent pins, retained diagnostics, blocked
reuse and quarantine after uncertain cleanup. New probe fixture tests prove
subprocess-mode close failures do not unwind/retry/unload. They do not qualify
Balluff's device ownership. Supervisor tests exercise process signals/timeouts
without any proprietary runtime.

### Executed local checks

All lanes configure/build with warnings-as-errors and `--parallel 2`; optional
lanes enable `GIGE_MEL_BUILD_GENTL_PROBE`, `GIGE_MEL_BUILD_GENTL_BACKEND`, examples,
the external header path and pinned Aravis `PKG_CONFIG_PATH`. Commands live in
the external `qualify.sh`, with per-lane logs and exit files. GCC is Debian
14.2.0-19; Clang is Debian 19.1.7. No new sanitizer suppressions.

| Lane | Result |
| --- | --- |
| Required `cmake --preset dev`, build preset dev, CTest preset dev | configure/build exit 0, **34/34 passed** |
| Optional GCC 14 | configure/build exit 0, **94/94 passed** |
| Optional Clang 19 | configure/build exit 0, **94/94 passed** |
| Optional GCC 14 ASan/UBSan, repaired host environment | configure/build/test exit 0, **94/94 passed**, leak detection/halt-on-error; `asan-isolated.log` |
| Optional GCC 14 TSan, repaired host environment | configure/build/test exit 0, **94/94 passed**, halt-on-error, no new suppressions; `tsan.log` |
| Optional backend `BUILD_TESTING=OFF`, repaired host environment | configure/build exit 0; consumer/backend archives have no `for_testing` or allocation-injection symbol |
| Final probe/supervisor changes, GCC and Clang | rebuilt probe, focused **2/2 passed** each (11 Python probe cases and 4 SDK-free supervisor cases) |
| Header-free default isolation | configure with GenTL OFF and nonexistent header path, provider build, six static isolation assertions: exit 0 |
| Provider binary isolation, GCC and Clang | zero GenTL/Aravis symbols; zero Aravis/GLib/libxml2 dependencies |
| `python3 scripts/vendor_upstream.py --verify` | exit 0, 97 pinned files |
| `python3 scripts/check_proprietary.py --history origin/main` | exit 0, index and new history guarded (369 entries at source commit) |
| Supervised final fixture, new external directory | namespace check, two load children, interface child all exit 0; **simulation only** |
| Original Balluff evidence integrity | all recorded log lengths and SHA-256 hashes reverified, original bundle unmodified |

The full GCC/Clang lanes preceded the final additional probe error-path regression
and external-child-working-directory adjustment. The changed paths were rebuilt
and retested specifically afterward; unrelated successful contracts were not
repeated merely to wait for hosted CI. Source commit for these final focused
checks: `4874bafd0a3fc0f18abe31873967bfc0d6fe2a4c`, tree
`0bae430190664ee562f9965455bb1cb59518f9eb`. Subsequent commits update this record.
The successful sanitizer and BUILD_TESTING=OFF checks used that committed source;
only this validation document was being updated during those runs.

**Host qualification repair, not a source workaround.** `/tmp` exhausted its
1,048,576 inode allowance (despite free byte capacity). Initial ASan/TSan builds
and BUILD_TESTING=OFF configure failed creating compiler temporaries; logs are
retained as `*-initial-tmp-full.log`. A first ASan retry built with external
`TMPDIR` but existing C++ fixtures hard-code `/tmp` for logs, so those assertions
failed on `mkstemp`, not on an ownership/sanitizer diagnostic. Those failures are
retained in `asan-tmp-inodes-full.log`/the first retry log; they are not passing
tests. No unrelated `/tmp` content was deleted. Final repaired checks use
`TMPDIR=/home/zboll/gentl-003-evidence/tmp` and a private user/mount namespace
binding that directory over **only the test processes'** `/tmp`. No host mount,
test-source contract, network interface or sanitizer suppression was changed:

```sh
unshare --user --map-root-user --mount -- bash -c \
  'mount --bind /home/zboll/gentl-003-evidence/tmp /tmp &&
   ctest --test-dir "$1" --output-on-failure' bash BUILD_DIRECTORY
```

An initial ad-hoc binary-isolation regex also falsely matched `GLIBCXX` when
searching for `glib`. GCC/Clang CTest had already passed 94/94; exact GenTL/Aravis
symbol and SDK dependency checks were rerun successfully without rebuilding
unrelated source. Original lane exit 1 from this inspection is preserved in
`gcc.exit`/`clang.exit`; corrected isolation logs and final focused results are
separate. No test failure is hidden by the corrected inspection.

Hosted CI is checked once after the final push and reported on the PR. Hosted
default/Aravis jobs are not the optional locally executed GenTL matrix and never
run the proprietary producer or physical camera. Independent technical review
is requested; passing tests are not merge approval.