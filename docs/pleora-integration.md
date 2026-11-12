# Optional Pleora eBUS backend (GIGE-MEL-026)

## Scope and qualification boundary

The internal Pleora adapter implements the existing `ICameraBackend`,
`ICameraDevice`, `IFrameStream` and typed `IDeviceFeatures` contracts using
eBUS **6.5.3.7155**, Linux x86_64. It consumes the same
`gige_mel_camera_profiles` implementation as Aravis. No camera-profile source,
MEL facade, installed header or factory selector is changed for Pleora.

This is a second-backend implementation awaiting native stream and physical
camera qualification. Compilation and native GenApi node tests do not establish
successful eBUS discovery, GVCP/GVSP acquisition, cancellation or disconnection
on a physical camera. The opt-in hardware test must pass on an Ethernet host
before treating the acquisition path as qualified. See the
[Debian 13 execution task](tasks/GIGE-MEL-026-pleora-debian13.md).

The default build remains offline and SDK-free. Set `GIGE_MEL_BUILD_PLEORA=ON`
explicitly to build the adapter, its diagnostic and native node tests. None is
installed or exported. The common provider retains its SDK-free dependencies;
`getAPI_Manager("synthetic")` remains the only standard factory selection.
Independent backend/profile selection and a stable plugin ABI remain later
steps in [camera-profile portability](camera-profile-portability.md).

## External development dependencies

The repository contains project-written adapter, tests and provisioning code.
It contains no Pleora or GenApi headers, samples, source, shared libraries or
SDK archives. All SDK files and their original notices stay outside the
repository and the installed provider package.

The repository owner selected the following mirror for internal development:

| Dependency | Pinned source |
| --- | --- |
| eBUS headers and original runtime | [RIVeR-Lab/ibex](https://github.com/RIVeR-Lab/ibex/tree/fa8139f75362eb377e97354f1eb079f042076b1a/docs/place_in_opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64), commit `fa8139f75362eb377e97354f1eb079f042076b1a`, subtree `docs/place_in_opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64` |
| GenApi advanced-access headers | Official [EMVA GenApi 3.4.1.1 archive](https://www.emva.org/wp-content/uploads/GenICam_V3_4_1_1_public_data.zip), SHA-256 `04cae9e8004edef0653c1af35522d04e01b01ac85962456b554c5894d904490a` |
| GenApi Linux SDK member | `GenICam_V3_4_1_1-Linux64_x64_gcc48-SDK.tgz`, SHA-256 `bc6287f516b46fe882cd2425fed334166360b275584ee932a25d271a4d9807eb` |

The eBUS mirror is not an official Pleora distribution. Its Ubuntu 22.04 package
name does not establish vendor support for Debian 13. The installed eBUS 6.5.1
used elsewhere on the camera host is a different version and is not replaced
or qualified by this work. The explicit helper preserves notices and installs
only in the chosen user-space prefix:

```sh
python3 scripts/provision_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel"
```

The helper downloads the pinned mirror only when that prefix does not exist.
It validates the eBUS version and required files, verifies both GenApi archive
checksums, copies the original SDK headers/runtime/notices, and extracts only
the separate GenApi headers. It writes `GIGE_MEL_SDK_PROVENANCE.json` in the
external prefix. A previously provisioned complete prefix is reused; an
incomplete or incompatible destination fails without replacement. To use
already available files:

```sh
python3 scripts/provision_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
  --sdk-source /absolute/path/to/existing/eBUS-6.5.3.7155 \
  --genicam-archive /absolute/path/to/GenICam_V3_4_1_1_public_data.zip
```

It performs no driver installation, system-library replacement, shell-profile
edit or network tuning. CMake and CI never invoke it.

### Why the extra GenApi headers are needed

`PvGenFloat` does not expose floating-point increment metadata in this SDK.
`PvGenParameterArray::GetNodeMap()` is the SDK's public advanced-access route
to the underlying `GenApi_3_4` node map. The adapter uses that route for typed
operations, including `IFloat::GetIncMode`, `HasInc` and `GetInc`.

The native service has one implementation for both the eBUS-owned node map
and an independently constructed test node map. The latter is loaded from
original project test XML by the **same bundled GenApi runtime**, without a
camera or network. It is not an eBUS transport simulation. A floating-point
list of permitted values cannot be represented by the current scalar-increment
contract and returns unsupported. Failed increment queries remain errors;
unknown values are never relabelled as absent increments.

The separate download supplies headers only. Link and run against the GenApi
libraries bundled with the selected eBUS installation, rather than replacing
them with the separately downloaded runtime. The bundle exports the
`GenApi_3_4` namespace; its exact patch version is not inferred from the
separate header package.

## Configure, build and test

```sh
cmake -S . -B build/pleora -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGIGE_MEL_BUILD_PLEORA=ON \
  -DGIGE_MEL_PLEORA_ROOT="$HOME/sdk/pleora-6.5.3.7155-gige-mel"
cmake --build build/pleora --parallel 2
ctest --test-dir build/pleora --output-on-failure
```

For an existing SDK with headers stored separately, also pass
`-DGIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR=/absolute/path/to/library/CPP/include`.
The configure checks require eBUS 6.5.3.7155, GenApi 3.4.1.1 headers, and the
SDK's original x86_64 GenApi runtime layout. Internal executables carry a
build-only runtime search path to this chosen SDK and its transitive libraries.
No SDK path or dependency is added to the installed CMake package.

The eBUS native loader requires `GENICAM_ROOT_V3_4` before `main()`, including
for `--help` and `--version`. CTest supplies the selected SDK paths and a build
directory cache automatically. For manual programs, use
`python3 scripts/run_pleora.py SDK_ROOT PROGRAM [ARGUMENTS]`. This launcher sets
the SDK environment for that process only. An embedding application must also
set up its chosen SDK environment before loading the adapter.

The normal Pleora build registers the existing SDK-free tests plus
`pleora_sdk_contract`, `pleora_diagnostic_help` and `pleora_sdk_version`.
The native node test exercises actual GenApi implementation, availability,
access, types, numeric metadata, alias selection, fresh constraints and shared
configuration orchestration. Its fixture supplies camera-like registers and
XML; it does not open a real `PvDeviceGEV` or receive a `PvBuffer` over Ethernet.

For project-code memory/undefined-behavior checks, configure another build with
`-DGIGE_MEL_SANITIZERS=address,undefined`. The closed SDK binaries are not
instrumented. Any runtime limitations or disabled checks must be recorded in
the qualification evidence, not silently treated as coverage.

## Identity, features and acquisition

Discovery returns canonical `mac:aa:bb:cc:dd:ee:ff` IDs from actual
`PvDeviceInfoGEV` records. Opening refreshes discovery and requires exactly one
matching path; it never selects the first camera or silently treats an IP
address as a stable device identity. Inspection records the current IP as a
transport fact. Duplicate paths are reported as ambiguous. Process-wide MAC
reservations prevent two adapter instances from controlling the same camera.

The shared profile preserves its existing first-implemented aliases,
microsecond exposure interpretation, integer-Hz rate requirement, ROI ordering,
actual readback and no-rollback diagnostics. Pleora does not silently normalize
trigger mode, automatic exposure, frame-rate enable, pixel format or selectors.
Mono8 and admitted unpacked Mono12 follow the same profile rules; Mono12 still
requires independently reported little-endian words. Packed formats remain
unsupported. Native `NO_LICENSE` results and images marked by
`PvImage::HasWatermark()` are rejected: the profile requires unmodified camera
pixels, so a watermarked evaluation image is not accepted as raw sensor data. No navigation transport or defect-map capability is fabricated.

Device operations hold one lock for the complete profile transaction. Native
reads/writes revalidate current implementation, availability, type and access;
dependent bounds are not cached authorization for later setters. Native query
exceptions and failed SDK results are translated to project errors. SDK types
and exceptions do not enter the shared profile or MEL-facing declarations.

Acquisition uses the native stream directly with a fixed pool of four buffers,
at most 64 MiB per buffer and 256 MiB per run. A delivered frame retains its
original native storage; it is not requeued while leased. Stop/close retire
native acquisition and preserve held bytes until lease release. Receive uses
bounded wait slices so a stop request can interrupt a long requested timeout.
Failed acquisition commands retain uncertain ownership until cleanup; an
ordinary frame timeout is not proof of a disconnect or a successful stop.
The local SDK `LinkRecoveryEnabled` policy is explicitly disabled and read
back before connecting. Confirmed native link loss is terminal for that
logical device and requires explicit close/reopen. These lifecycle paths require the native hardware
qualification described below.

Production `FrameTimestamp{}` remains invalid. A raw eBUS block timestamp is
not an established integration-start time or UTC mapping. The common provider
therefore continues to reject these frames at its production time gate.
Positive MEL tests use a separate, clearly labelled test-only virtual clock;
that decorator is never linked into the adapter or diagnostic.

## Diagnostic and hardware execution

```sh
python3 scripts/run_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
  build/pleora/gige_mel_pleora_diagnostic --version
python3 scripts/run_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
  build/pleora/gige_mel_pleora_diagnostic list
python3 scripts/run_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
  build/pleora/gige_mel_pleora_diagnostic inspect mac:aa:bb:cc:dd:ee:ff
python3 scripts/run_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
  build/pleora/gige_mel_pleora_diagnostic capture mac:aa:bb:cc:dd:ee:ff 16 10000
```

Replace the example MAC with the exact discovery result. Help/version perform
no camera discovery. Inspection reads camera facts. Capture starts/stops native
acquisition, negotiates packet size, sets the native stream destination and reports validated frame
dimensions, IDs, byte checksums and statistics. It does not modify
ROI/exposure/rate/trigger/pixel-format settings or save image files. Its timeout
is one overall **receive** deadline; SDK discovery/open/cleanup have their own
bounds. `apply` is a separate explicit command with a complete requested
configuration; use `--help` for its argument order.

To register the opt-in hardware contract:

```sh
cmake -S . -B build/pleora -G Ninja \
  -DGIGE_MEL_BUILD_PLEORA=ON \
  -DGIGE_MEL_PLEORA_ROOT="$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
  -DGIGE_MEL_PLEORA_HARDWARE_TESTS=ON \
  -DGIGE_MEL_PLEORA_DEVICE_ID=mac:aa:bb:cc:dd:ee:ff
cmake --build build/pleora --parallel 2
ctest --test-dir build/pleora -R '^pleora_hardware_contract$' --output-on-failure
```

This test is off by default and fails on unavailable hardware or unsupported
configuration; it never turns those failures into a skipped success. It uses
the selected camera's current profile, tests source pool/stop/lease/reopen
behavior, and checks the MEL timestamp gate and isolated virtual-clock path.
It does not qualify physical timing or camera unplug behavior.

## Qualification record

The implementation workspace runs Debian 13, GCC 14.2 and the pinned eBUS
runtime. The following checks were executed for this implementation:

| Check | Result and limit |
| --- | --- |
| Default development build | 11/11 SDK-free CTest tests passed |
| Optional Pleora development build | 14/14 CTest tests passed, including all eight actual native feature-test groups |
| Optional Pleora ASan/UBSan build | 14/14 passed; project code instrumented; local LeakSanitizer disabled because this workspace runs under process tracing; vendor binaries uninstrumented |
| SDK version | Compiled and loaded eBUS both report 6.5.3.7155 |
| Optional physical test | Compiled with strict project warnings; not executed; absent from normal test registration |
| Installed provider/package | No SDK/GenApi dependencies in provider, native symbols or public package text; independent installed consumer built and passed |
| Provisioning | Pinned-mirror download path, checksum-verified header extraction, existing-source provisioning and complete-prefix reuse executed |
| Source preservation | Shared profile, common provider, installed interfaces and pinned upstream files unchanged |

The ASan/UBSan run used `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. This is not a successful leak
check. The existing hosted default/Aravis sanitizer settings remain unchanged;
hosted CI does not download or test the Pleora SDK.

Native eBUS software-device/receiver probes were executed but found no
usable Ethernet interface: `PvVirtualDeviceGEV::StartListening("127.0.0.1")`
reported `NOT_FOUND`, and `PvStreamGEV::Open` reported no suitable data receiver.
Only loopback is exposed and network-interface administration is unavailable.
This is an environment limit, not successful stream qualification.

Physical Cognex CIC-5000R, host NIC, eBUS driver/user-mode transport, loss and
recovery, throughput, source-buffer ownership under traffic, and Pleora-pixel
MEL publication remain pending until the explicit host test is executed and
its evidence reviewed. Existing Aravis CI does not cover the new Pleora path.
