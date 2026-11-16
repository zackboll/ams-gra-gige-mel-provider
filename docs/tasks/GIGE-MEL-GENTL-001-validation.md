# GIGE-MEL-GENTL-001 — validation record

Base: fetched `main` `60e1fbd` (PR #36 merge). Branch
`feature/gige-mel-gentl-001-feasibility`. The final commit, tree and hosted CI
disposition are recorded on the PR, to avoid a self-referential commit identity
here. **Final recommendation: `CONDITIONAL_GO`** (design section 9).

Evidence classes are kept separate. **F** = controlled fixture producer
(offline; proves consumer ownership and error logic only). **P** = real
third-party producer, offline (no device). **H** = real producer with the
physical Cognex CIC-5000R. Nothing in F or P qualifies a camera, producer
streaming or timing.

## A. Environment and exact sources

| Item | Version / identity |
| --- | --- |
| Host | Debian 13 (trixie), x86-64, GCC 14.2.0-19, CMake 3.31.6, Ninja 1.12.1, Python 3.13.5 |
| GenTL header | EMVA `GenICam_Package_2026.07.zip`, SHA-256 `e74b8d982c3e7697399b03c254b6e8c596eddf0279959eba7de18734e279b64f` (matches emva.org); member `GenTL/GenTL.h`, SHA-256 `5b656a413f4cdc347e80494b87f94b09ac925e391a9022edb7f028d996e35e4e`, GenTL 1.6 |
| Aravis (Approach A source, producer) | Tag `0.9.2`, commit `c1a23267e25c64425b147da88b657c23a3cd7b1f`; `aravis-0.9.2.tar.xz` SHA-256 `3bf0e79e8978c561d5af04c9958350f91fdeecaa2c2d96ff48420f0ae017af86`; its `src/gentl/GenTL_v1_6.h` is byte-identical to the EMVA header |
| Aravis producer build | meson `--buildtype release -Dviewer=disabled -Dgst-plugin=disabled -Dusb=disabled -Dpacket-socket=disabled -Dintrospection=disabled -Ddocumentation=disabled -Dtests=false -Dgentl-producer=true`; `ProducerAravis-0.10.cti` SHA-256 `0ca61b9e69062b3167a4321a77a177db5f00a3036c76e5f8abbda740a2a1f103` |
| Balluff Impact Acquire | `ImpactAcquire-x86_64-linux-3.7.2.sh` (static.matrix-vision.com, 2026-07-28), SHA-256 `8d54f2ca366b5d01d6e965afa505beac9d2aa84bf960f03396766f6a744a80ca`; payload MD5 `dc7c94d30707cf4e11fc641f1e19a4b5` (equals the embedded `TARFILE_CRC`); `libmvGenTLProducer.so.3.7.2` SHA-256 `247d68e2ab1be3155889029cc361e8688b2084de03d3fde0b52f967d884ee9e4`; `TL_INFO_VERSION 3.7.2.4495`. Installer **not executed** (no root, udev or `/etc` changes). |
| Euresys eGrabber/Gigelink | 26.10 `egrabber-linux-x86_64-26.10.0.11.tar.gz` **not obtained**: CMS API `403 "File is protected, please authenticate to download."`. Release notes `egrabber-release-notes-26.10.0.2224.pdf` (SHA-256 `d42acdf23dd40faa5b632c9cf49e44a6ab9064fdc946782d87e7f832fb00f636`) downloaded anonymously. |
| Camera | Cognex CIC-5000R, serial 21874533, 192.168.24.101 via `enp5s0` (see the [Cognex record](../cognex-cic-5000r.md)) |

All third-party downloads and builds live outside the repository
(`~/sdk/gentl-001`). No proprietary file, header or binary is committed.

## B. Default build (SDK-free; behavior unchanged)

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev
python3 scripts/vendor_upstream.py --verify
python3 scripts/check_proprietary.py --history origin/main
```

Results are in section G. The default configuration never references the GenTL
header, fixture or probe: `GIGE_MEL_BUILD_GENTL_PROBE` defaults to OFF. The new
SDK-free `gentl_default_isolation` test checks that default; that the GenTL module
is the only gated include; that the module never installs or links the
provider or selection; that no provider, factory, selection or installed header
mentions GenTL; and that no GenTL header or `.cti` file is tracked.
## C. GenTL feasibility build and fixture tests (class F)

```sh
scripts/provision_gentl_header.sh ~/sdk/gentl-001/header --source emva \
  --archive ~/sdk/gentl-001/GenICam_Package_2026.07.zip   # --source aravis also verified identical
cmake --preset dev -B build/gentl-dev -DGIGE_MEL_BUILD_GENTL_PROBE=ON \
  -DGIGE_MEL_GENTL_INCLUDE_DIR=$HOME/sdk/gentl-001/header/include
cmake --build build/gentl-dev --parallel 2
ctest --test-dir build/gentl-dev --output-on-failure
```

`gentl_consumer_contract` runs 19 scenarios with real `dlopen`/`dlsym` against
three fixture `.cti` builds:

* Paths: empty, relative, missing, directory and non-ELF paths.
* Missing mandatory symbol: the producer is loaded, `GCInitLib` is **never**
  called, and the library is unloaded.
* `GCInitLib` failure: native code kept, no `GCCloseLib`, library unloaded.
* Five full load/close cycles in strict DevClose → IFClose → TLClose →
  GCCloseLib → unload order, with idempotent double close.
* Parent close refused, with no native call, while children are open.
* Destructor-only teardown order.
* Exact selection: no prefix, case or serial matching, and no implicit discovery.
* A partial-open failure closes the opened device.
* Native code and `GCGetLastError` text propagate.
* One instance per producer file; all-or-nothing optional symbol groups.
* XML location, `Local:` parsing and read; read-only inspection issues no
  `GCWritePort`.
* Register words follow the declared port byte order (big and little), refuse an
  undeclared or contradictory declaration, validate before the native call, and
  report `uncertain` after a fixture write that applied bytes and then timed out.
* Streams: start, receive, starvation without overwriting held leases, stop,
  restart refused while a lease is held, three restarts with balanced
  announce/revoke.
* Leases stay readable after real producer unload (checked with `RTLD_NOLOAD`).
* `request_stop` wakes a blocked infinite wait through `EventKill`.
* Cleanup after each start failure; payload and memory bounds checked before
  allocation.
* A failed destructor close taints the producer; a failed revoke quarantines
  memory; a failed `GCCloseLib` retains the library permanently.

`gentl_probe_fixture` drives the real probe executable:

* usage and gating: `acquire` is refused before anything loads unless
  `--allow-host-acquisition` is given;
* path and symbol failures, and native errors reported verbatim;
* `interfaces` performs no device discovery;
* `inspect` is read-only and opens exactly the requested device with
  `DEVICE_ACCESS_READONLY`;
* `--repeat 3` acquire with `--reopen 2`: six stream cycles, balanced
  announce/revoke, three loads and unloads;
* a stalled acquisition times out without hanging;
* `--no-interface-info`.

Sanitizer lanes cover the GenTL targets and both GenTL tests; the fixture
producer is instrumented too:

```sh
cmake --preset asan -B build/gentl-asan -DGIGE_MEL_BUILD_GENTL_PROBE=ON -DGIGE_MEL_GENTL_INCLUDE_DIR=...
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/gentl-asan -L gentl --output-on-failure
cmake --preset tsan -B build/gentl-tsan -DGIGE_MEL_BUILD_GENTL_PROBE=ON -DGIGE_MEL_GENTL_INCLUDE_DIR=...
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/gentl-tsan -L gentl --output-on-failure
```

## D. Real producers, offline (class P)

| Command | Producer | Result |
| --- | --- | --- |
| `gige_mel_gentl_probe <cti> --repeat 3 load` | Aravis 0.9.2 | exit 0. `TL_INFO_ID = Aravis-ID`, `TL_INFO_GENTL_VER_MAJOR/MINOR = 0/1`. Three clean load/init/close/unload cycles. |
| `unshare --user --map-root-user --net … interfaces` | Aravis 0.9.2 | **exit 139 (SIGSEGV)** inside the producer: `TLGetInterfaceInfo → gentl_to_buf → strlen(NULL)`. Every `TLGetInfo` returns `GC_ERR_NOT_IMPLEMENTED`. |
| same, with `--no-interface-info` | Aravis 0.9.2 | exit 0; interfaces `Fake`, `GigEVision`, `GenTL` |
| `--repeat 2 load` (Balluff env: `MVIMPACT_ACQUIRE_DIR`, `GENICAM_ROOT[_V3_5]`, `GENICAM_CACHE_V3_5`, `LD_LIBRARY_PATH`) | Balluff 3.7.2 | exit 0. `TL_INFO_VENDOR = Balluff`, `TL_INFO_VERSION = 3.7.2.4495`, GenTL 1.6, every optional symbol present. |
| `unshare … --net … interfaces` (no `lo` link) | Balluff 3.7.2 | **exit 139 (SIGSEGV)** inside `TLUpdateInterfaceList` |
| same, after `ip link set lo up` | Balluff 3.7.2 | exit 0. Five `U3V` xHCI interfaces and no GEV interface (shipped `ignoredInterfaces.txt` has `GEV=I`). |
| Aravis producer `inspect` against `arv-fake-gv-camera-0.10 -i 127.0.0.1` in an isolated namespace | Aravis 0.9.2 | Open succeeded with `DEVICE_ACCESS_READONLY`, yet the GVCP trace shows `write-register-cmd 0x0A00 = 2` (CCP control). All `DevGetInfo`/`GCGetPortInfo` calls return `GC_ERR_NOT_IMPLEMENTED`, so the port byte order is undeclared and registers are refused. URL `Local:////arv-fake-camera.xml;10000;3e6f`. `GCReadPort` of the XML failed with `GC_ERR_IO` (stale `GCGetPortURLInfo` error text). `DevClose` returned `GC_ERR_NOT_IMPLEMENTED`; the probe tainted and retained the producer and exited 1. |
## E. Approach A consumer against the fixture (class F, outside the repository)

`~/sdk/gentl-001/approach-a/aravis_consumer_probe.c`, linked against
`aravis-0.10` 0.9.2, was run with `arv_disable_interface("GigEVision")` and
`("Fake")`, `GENICAM_GENTL64_PATH` set to a directory containing only the fixture,
and an isolated network namespace. Exit 0: two devices (`mock-dev-A`/`-B`,
protocol `Custom`); `arv_open_device("mock-dev-B")` returned `ArvGenTLDevice`;
GenApi `Width = 8`; `arv_device_read_register(0x100)` returned raw `0x08000000`
for a big-endian 8 (no byte-order conversion); `arv_shutdown` completed. Fixture
log: `GCInitLib`, two TL/IF enumeration cycles, `IFOpenDevice … access=3`
(`CONTROL`), XML read of 766 bytes, two 4-byte reads, then `DevClose`,
`IFClose`, `TLClose`, `GCCloseLib` and `unloaded`. Before the fixture exported
the 12 optional GenTL 1.4/1.5 stubs, Aravis rejected it with
`undefined symbol: IFGetInfo` and found zero devices.

## F. Physical camera (class H): Option B scope only

The user authorized read-only discovery and inspection of the CIC-5000R
**through the Aravis GenTL producer only**: no configuration, acquisition or
register writes, no interference with another controller, and no Gigelink or
Balluff camera contact. Before contact, `ps` showed no Aravis, eBUS, Impact or
probe process, and no UDP socket was bound to port 3956.

| UTC | Command | Result |
| --- | --- | --- |
| 2026-11-16T04:30:32Z | `gige_mel_gentl_probe ProducerAravis-0.10.cti --no-interface-info discover GigEVision` | exit 0. One device: `COGNEX-CIC-5000R-21874533`, vendor `COGNEX`, model `CIC-5000R`, serial `21874533`, TL type `GigEVision`, access status `1` (READWRITE = available). |
| 2026-11-16T04:37:30Z | same with `ARV_DEBUG=cp:4` | exit 0. GVCP trace: exactly two `discovery-ack` packets and **no other command**; no register or memory traffic. Identical identity. |

**Open/inspect of the Cognex was deliberately not performed.** Section D showed
that the Aravis producer's `IFOpenDevice` ignores `DEVICE_ACCESS_READONLY`, writes
the CCP control-privilege register (`0x0A00 = 2`), starts a heartbeat, and cannot
release the device (`DevClose` is unimplemented). That exceeds the authorized
read-only scope and could interfere with a future controller. As a result, port,
XML and feature inspection through GenTL, configuration readback, Mono8
acquisition, 10 Hz operation, buffer lifecycle, stream stop and reopen are all
**not tested on hardware**.

| Required check | Status |
| --- | --- |
| Discovery (correct identity) | **Done** (H, Aravis producer): matches the prior record (`DeviceID`/serial 21874533) |
| Open/close | Not done: no producer that opens read-only and closes correctly was available |
| Feature inspection | Not done on hardware; fixture XML and GenApi only (F, and A in section E) |
| Configuration readback | Not done |
| Mono8 acquisition / 10 Hz | Not done (fixture Mono8 only) |
| Buffer lifecycle / stop / reopen | Fixture only (F) |

Raw logs, outside the repository: `~/gige-mel-evidence/gentl-001/`.
## G. Executed results (final source)

Local run on the final source tree (script `/tmp/gentl001-final.sh`, run serially;
each exit status was captured individually). The exact final HEAD rerun and the
hosted CI result are recorded on the PR.

| Lane | Command | Exit | Result |
| --- | --- | --- | --- |
| Default dev (GCC 14.2) | `cmake --preset dev && cmake --build --preset dev --parallel 2 && ctest --preset dev` | 0 / 0 / 0 | 30/30 passed, including the new `gentl_default_isolation` |
| Pinned upstream | `python3 scripts/vendor_upstream.py --verify` | 0 | 97 pinned files verified |
| Proprietary guard | `python3 scripts/check_proprietary.py --history origin/main` | 0 | no prohibited material |
| GenTL dev (GCC) | `build/gentl-dev`, full CTest | 0 | 32/32 passed: the 30 default tests plus `gentl_consumer_contract` and `gentl_probe_fixture` |
| GenTL ASan+UBSan | `build/gentl-asan`, `ctest -L gentl` | 0 | 2/2 passed, leak detection on |
| GenTL TSan | `build/gentl-tsan`, `ctest -L gentl` | 0 | 2/2 passed |
| GenTL Clang 19.1.7 | `CXX=clang++`, `build/gentl-clang`, `ctest -L gentl` | 0 | 2/2 passed |

Hosted CI does not provision the GenTL header, so the GenTL lanes are local only;
hosted CI exercises the default-OFF build and `gentl_default_isolation`.

## H. Unqualified behavior and untested assumptions

* No real producer has opened the Cognex, read its XML through GenTL, executed a
  GenApi feature, configured it or delivered a frame. Every acquisition,
  buffer-lifecycle and stop/reopen claim is fixture-only.
* Gigelink (preferred producer) was not obtained. Its free-of-charge status is
  documentary (26.10 release notes), not executed.
* Balluff's third-party-hardware licence was not granted, and its GEV interface
  is ignored by default configuration; no GEV discovery was attempted with it.
* The GenApi layer (design section 7) is not implemented. The `ArvGc`-over-GenTL
  option is assessed from Aravis 0.8.36 headers and source only.
* `EventKill` waking a blocked wait is proven only for the fixture. Producers may
  differ, which is why the production adapter should use bounded wait slices.
* GenTL timestamps are reported raw with an UNKNOWN clock domain. No
  integration-start, host or UTC meaning is assigned, and none is manufactured.
* Port writes: no packet-count, retry, deadline or sensor-acceptance property is
  established for any producer. NavigationReport transport remains gated.
* Producer crashes (Aravis `TLGetInterfaceInfo`; Balluff `TLUpdateInterfaceList`
  without `lo`) are reproducible but were not reported upstream in this task.
* The consumer is single-threaded apart from `request_stop` and the
  device-locked register/port operations. A production adapter must reapply the
  existing receive, stop and close threading contracts.
* Retained (tainted) producers leak their mapping and quarantined memory for the
  rest of the process, by design.
