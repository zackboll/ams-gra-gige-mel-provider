# Cognex CIC-5000R profile v1

Independently built, SDK-free Linux C++20 module; canonical selection ID
`cognex_cic_5000r_v1`. It uses the existing matched-build module v1 descriptor
and declares compatibility with both Aravis and Pleora. It neither links the
provider nor includes/links a camera SDK. This is trusted matched-C++ code, not
a stable cross-toolchain plugin ABI. See [authoring](../../docs/camera-profile-authoring.md).

## Build and select

Use the **same compiler, configuration, ABI flags, interface revision and
sanitizers as the host** (including `CMAKE_CXX_FLAGS_<CONFIG>` when customized):

```sh
cmake -S /absolute/checkout/camera_profiles/cognex_cic_5000r \
  -B /absolute/checkout/build/cognex-standalone -G Ninja \
  -DGIGE_MEL_SOURCE_ROOT=/absolute/checkout \
  -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_BUILD_TYPE=Debug
cmake --build /absolute/checkout/build/cognex-standalone --parallel 2
nm -D --defined-only /absolute/checkout/build/cognex-standalone/libcognex_cic_5000r.so
readelf -d /absolute/checkout/build/cognex-standalone/libcognex_cic_5000r.so

gige_mel_select \
  --profile-module /absolute/path/libcognex_cic_5000r.so \
  aravis EXACT_ARAVIS_DEVICE_ID cognex_cic_5000r_v1
gige_mel_select \
  --profile-module /absolute/path/libcognex_cic_5000r.so \
  pleora EXACT_DISCOVERED_MAC_ADDRESS cognex_cic_5000r_v1
```

For Pleora supply the exact discovered lowercase MAC (no `mac:` prefix), and
its documented external SDK runtime environment. These commands load/register/
select only: **no discovery, camera open, writes or acquisition**. `--inspect`
explicitly requests open/read/close; application initialization also opens.
Neither authorizes configuration or capture. A disabled power-on rate control
will explicitly fail open; this module never enables it. Pleora entitlement and
licensing requirements remain unchanged; selection does not imply permission.

## Admission and configuration

At explicit open, read `DeviceVendorName=COGNEX`,
`DeviceModelName=CIC-5000R`, and
`DeviceFirmwareVersion=106349-08;U;CIC-5000R;V3.7-1;0` exactly. Missing,
unreadable or different identity fails before configuration discovery/writes;
native failures retain their code/text with the feature name. No serial number
restriction or broad firmware pattern; new firmware needs explicit evidence.
No generic fallback.

The required native representation is **actual Mono8**, with
`AcquisitionFrameRateAbs` in Hz, `ExposureTimeAbs` in microseconds,
`AcquisitionFrameRateEnable=true`, `TriggerMode=Off`, and
`AcquisitionMode=Continuous`. Shared manual-exposure/ROI checks still apply.
Open, apply and acquisition preflight never normalize these values. Mono12 is
unavailable even when an artificial fixture declares Little byte order.

Only **1, 2, 4, 5, 8, 10 Hz** requests/runs are admitted. Capabilities advertise
at most 10 Hz; that scalar envelope does not mean every intermediate integer is
admitted. Reject 12, 14 and even exactly representable 16 Hz before writes.
This is conservative initial policy, not a physical limit. Native configuration
readback is never clamped to the policy: e.g. 16 Hz remains readable and can be
explicitly corrected. A fractional native rate cannot fit the existing integer
configuration type and returns its actual value in the shared diagnostic.

The owned state inherits capabilities and readback from
[`MonoFreeRunningProfileState`](../../src/profiles/mono_free_running_profile.hpp),
which keeps the native admission facts distinct from the advertised bounds.
The profile constructs the Cognex state explicitly after model admission.
Cognex adds native checks before the default write transaction, admits the
already-read snapshot in its own preflight sequence, and retains model-specific
diagnostics and the Mono8-only frame gate. Shared SDK-free algorithms retain
fresh bounds, complete-request validation,
ROI/exposure conversion, ordered writes, authoritative readback and partial-write
diagnostics with **no rollback**. No pixel-format, trigger, exposure-auto,
persistence, transport or host-network writes are added. Native frame IDs and
generic layout/span validation are unchanged; there is no synthetic parity rule.

## Evidence and limits

The [physical record](../../docs/cognex-cic-5000r.md) is the evidence source:
V3.7-1 quantizes periods in microseconds; exactly representable integer rates
up to 16 are 1, 2, 4, 5, 8, 10, 16. Strongest prior streaming evidence is
full-resolution 2592×1944 Mono8 at 2, 5 and 10 Hz on the observed 1 Gb/s link.
Heavy-load loss was also observed. Admission/exact representation does **not**
guarantee loss-free transport. Other rates, ROI and this new module have no new
physical qualification. Mono12 native byte order remains unqualified.

The [qualification record](../../docs/tasks/GIGE-MEL-036-validation.md) separates
SDK-free typed fixtures, native Aravis Fake, native Pleora GenApi/PvBuffer tests
and physical evidence. No physical camera is accessed by these tests. Production
timestamps stay invalid; no optics, calibration, navigation or UTC is inferred.
Hardware work is optional and requires separate explicit approval: identify
read-only first, snapshot initial writable state, approve temporary settings,
run SDKs sequentially, restore and independently verify every modification.