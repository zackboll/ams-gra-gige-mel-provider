# GIGE-MEL-027: qualify the canonical Pleora backend on Debian 13

This task is intended for Cline running on Zack's Debian 13 camera host. It
continues canonical PR #26 (`feature/gige-mel-025-pleora-backend`). Historical
watermarked Mono8 evaluation is not licensed production qualification.

## Objective and constraints

Build the optional Pleora backend against eBUS 6.5.3.7155, run its native feature
tests, and qualify the current Cognex CIC-5000R configuration through the same
shared camera profile used by Aravis. The expected installation has camera IP
`192.168.24.101` and host `192.168.24.10/24` on `enp5s0`; verify these facts before
using them. Select the camera by its discovered MAC ID and record its reported
model/serial/firmware.

Zack authorized using the pinned RIVeR-Lab/ibex SDK mirror for internal
development and hardware testing. Keep all downloaded SDK headers, samples,
libraries and notices outside the Git repository. Do not commit or upload SDK
files, camera images or private host evidence. Do not replace an existing eBUS
6.5.1 installation. Read the repository's `AGENTS.md` and
[Pleora integration contract](../pleora-integration.md) first.

## Execute

1. Check out this task's Pleora PR branch and record its commit and clean/dirty
   state. Preserve unrelated local work. Verify the host architecture and
   toolchain, and record `ip -brief address show enp5s0`. Do not change addresses,
   routing, MTU, kernel settings, services or drivers as a side effect of this
   task. Identify and close only a camera-owning process that Zack has directed
   you to stop; do not kill unrelated applications.

2. Establish SDK development entitlement and receiver licensing before physical
   qualification. The default hardware contract has no evaluation switch; do not
   weaken it to admit watermarked frames. If entitlement is unresolved, record
   physical qualification as pending. Download authorization is not a license.

   Use an external user-space SDK prefix. If this task's complete prefix is
   absent, the explicit helper downloads the authorized mirror and matching
   official GenApi headers. It verifies the pinned revision/checksums and
   preserves notices. It does not request a new download authorization.

   ```sh
   python3 scripts/provision_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel"
   ```

   If the helper reports an existing incompatible/incomplete prefix, inspect it
   and choose a new external prefix. Do not delete or overwrite the installation.
   Use `--sdk-source` if a complete 6.5.3.7155 SDK already exists elsewhere. Keep
   the SDK's own GenApi runtime; the extra download supplies headers only.

3. Configure and build without selecting any physical camera, then run the
   SDK-free/native-node tests:

   ```sh
   cmake -S . -B build/pleora -G Ninja \
     -DCMAKE_BUILD_TYPE=Debug \
     -DGIGE_MEL_BUILD_PLEORA=ON \
     -DGIGE_MEL_PLEORA_HARDWARE_TESTS=OFF \
     -DGIGE_MEL_PLEORA_SDK_ROOT="$HOME/sdk/pleora-6.5.3.7155-gige-mel"
   cmake --build build/pleora --parallel 2
   ctest --test-dir build/pleora --output-on-failure
   python3 scripts/run_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
     build/pleora/gige_mel_pleora_diagnostic --version
   ```

   The explicit hardware-test OFF setting prevents a previous CMake cache from
   selecting an old physical camera before fresh discovery. Investigate missing
   system runtime dependencies rather than copying
   incompatible libraries into the provider. Record any required host package
   change separately from SDK provisioning.

4. Run `list`, identify the expected physical camera, and then `inspect` its
   exact MAC ID. Save text evidence under ignored `camera-evidence-pleora/`.
   Record `compiled_ebus` and `runtime_ebus`, camera identity, current ROI,
   exposure, rate, pixel format, trigger/auto controls, and any profile rejection.
   A missing/unsupported/unreadable feature is a real result; do not modify the
   camera, profile or tests just to make inspection pass.

   ```sh
   mkdir -p camera-evidence-pleora
   python3 scripts/run_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
     build/pleora/gige_mel_pleora_diagnostic list
   python3 scripts/run_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
     build/pleora/gige_mel_pleora_diagnostic inspect aa:bb:cc:dd:ee:ff
   ```

5. With the exact ID substituted and a supported current camera configuration,
   capture 16 frames, then enable and execute the explicit hardware contract.
   Acquisition configures transport destination and issues start/stop commands (no packet negotiation);
   the test does not normalize camera ROI/exposure/rate/trigger/pixel format.

   ```sh
   python3 scripts/run_pleora.py "$HOME/sdk/pleora-6.5.3.7155-gige-mel" \
     build/pleora/gige_mel_pleora_diagnostic capture-current aa:bb:cc:dd:ee:ff 16
   cmake -S . -B build/pleora \
     -DGIGE_MEL_PLEORA_HARDWARE_TESTS=ON \
     -DGIGE_MEL_PLEORA_DEVICE_ID=aa:bb:cc:dd:ee:ff
   cmake --build build/pleora --parallel 2
   ctest --test-dir build/pleora -R '^pleora_hardware_contract$' --output-on-failure
   ```

   Preserve command exit status when capturing output, for example with
   `set -o pipefail` before a `tee` pipeline. A failed receive/stop/cleanup or a
   skipped test is not a successful qualification. Do not automatically retry
   uncertain acquisition commands or add reconnection/rollback behavior.

6. Build an ASan/UBSan variant with the same external SDK and run the native
   tests, including the explicitly selected hardware contract where the host
   runtime supports it. Record project-code coverage separately from the
   uninstrumented vendor binaries. Preserve full first-failure output.

7. Reinspect the current camera state after cleanup. Report the exact source
   commit, SDK/header provenance, host/toolchain, camera identity, every executed
   test result and unresolved failures. Summarize host-copy lease retention,
   full-pool behavior, bounded cancellation, close/reopen, production timestamp
   rejection and test-only MEL delivery. Do not claim UTC or integration-start
   timing from the virtual clock, or unplug recovery from normal stop/reopen.

## Completion evidence

The licensed native acquisition gate is ready for independent review only when the selected hardware
contract and applicable sanitizer checks pass with the unchanged shared profile,
held bytes remain stable through stop/close, the default package stays SDK-free,
and all failures or environment limits are explicit. Keep detailed private logs
on the host; add only a reviewed, non-sensitive summary to the PR. Do not merge
or publish SDK files as part of this execution task.
