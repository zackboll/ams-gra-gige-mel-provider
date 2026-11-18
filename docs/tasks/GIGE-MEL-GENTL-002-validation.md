# GIGE-MEL-GENTL-002 — Validation record

This record accompanies the [design](GIGE-MEL-GENTL-002-design.md). Every result
below is **offline**, using the controlled GenTL fixture producer and a
**simulated** GenICam register model. No physical camera, network camera, vendor
SDK or third-party GenTL producer was contacted. Nothing here qualifies
real-camera GenTL operation.

## A. Environment and source versions

| Item | Value |
| --- | --- |
| Base | `main` `c6d555d4e841eb8ed576615d7716cb54bfebfb28` (PR #39 merged) |
| Host | Debian, Linux x86-64 |
| Compilers | GCC 14.2.0 (`/usr/bin/c++`), Debian Clang 19.1.7 (`clang++-19`) |
| CMake | 3.31.6 with Ninja |
| Aravis | 0.8.36 via `scripts/provision_aravis.sh` (archive SHA-256 `246deaa0042a387ff1bc00332d0fb80537ce14abde2c28d1a54c91f17adc51bf`, tag commit `97ff5e330919d7a44b871df4eda669feeafdf342`), uninstrumented |
| libxml2 / zlib | 2.9.14 / 1.3.1 (system; Aravis's own dependencies) |
| GenTL header | EMVA GenTL 1.6, SHA-256 `5b656a41…6e35e4e`, verified at configure time |
| Producer | `gentl_mock_producer.cti` (project fixture only) |
| Cognex profile | unchanged `camera_profiles/cognex_cic_5000r`, built by the existing ExternalProject with the host's flags and sanitizers |

## B. Final-commit qualification

Each lane below was run from a clean `git worktree` of the final source commit
recorded on the PR. All builds use the `dev` preset with `-Werror`. "GenTL"
lanes set `GIGE_MEL_BUILD_GENTL_PROBE=ON`, `GIGE_MEL_BUILD_GENTL_BACKEND=ON` and
`GIGE_MEL_BUILD_EXAMPLES=ON`.

Source commit `e4e464097e5b174d3657256d130de77134d68cb7` (tree
`ac083042eea83cecd5d4b4c0b9960b992415df3f`). The final PR head adds only this
record and the documentation commit; it was re-guarded as described on the PR.

| Lane | Configuration | Result |
| --- | --- | --- |
| Default | `cmake --preset dev` (no GenTL/Aravis/Pleora) | 31/31 passed |
| GenTL | GCC 14, GenTL lanes | 81/81 passed (incl. 9 new backend/example tests, 39 GENTL-001 R1 cases) |
| ASan/UBSan | GenTL lanes, `GIGE_MEL_SANITIZERS=address,undefined` | 81/81 passed |
| TSan | GenTL lanes, `GIGE_MEL_SANITIZERS=thread` (no new suppressions) | 81/81 passed |
| Clang 19 | GenTL lanes, `CMAKE_CXX_COMPILER=clang++-19` | 81/81 passed |

Additional configurations executed:

* `BUILD_TESTING=OFF` with the backend ON: configure and build succeed, and `nm` finds
  no `*_for_testing` symbol in `libgige_mel_gentl_backend.a`.
* Backend ON without the probe: configure fails with "requires
  GIGE_MEL_BUILD_GENTL_PROBE=ON".
* Backend ON without Aravis on `PKG_CONFIG_PATH`: configure fails with "requires
  exactly Aravis 0.8.36". Nothing is downloaded.
* Provider isolation, checked in both the default and the backend-ON build.
  `libgige_mel_provider.so` has:
  * 0 GenTL/Aravis dynamic symbols;
  * 0 Aravis/GLib/libxml2 `NEEDED` entries;
  * no `.cti` or `GCInitLib` strings.

  Default provider initialization therefore cannot load a producer.
* `python3 scripts/vendor_upstream.py --verify`: 97 pinned files verified.
* `scripts/check_proprietary.py --history origin/main`: no prohibited material.

The same matrix was also run on the pre-commit working tree, with identical
results. Hosted CI runs only the default build and does not substitute for the
optional GenTL/Aravis lanes above.

## B.1 GIGE-MEL-GENTL-002-R1 final-source qualification

The R1 correction was qualified from a clean detached `git worktree` of source commit
`7258195f0eb4264f841b00db5fa637e227697ef1` (tree `b2de1b239f8a7b564116ebcee891e3001fb94ee4`).
That commit incorporates `main` `ac31074` (PR #41, GIGE-MEL-042). The only merge conflicts
were documentation-only, in `implementation-profile.md` and `mel-surface-roadmap.md`, and both
sections were kept. The environment is the same as in §A. The PR head after this commit changes
only this record.

| Lane | Configuration | Result |
| --- | --- | --- |
| Default | `cmake --preset dev`, `cmake --build --preset dev --parallel 2`, `ctest --preset dev` | 33/33 passed (`GIGE_MEL_BUILD_GENTL_BACKEND=OFF`) |
| GenTL | GCC 14, GenTL lanes | 93/93 passed |
| ASan/UBSan | GenTL lanes, `address,undefined` | 93/93 passed, no sanitizer report |
| TSan | GenTL lanes, `thread`, no new or changed suppressions | 93/93 passed, no ThreadSanitizer warning |
| Clang 19 | GenTL lanes, `clang++-19` | 93/93 passed |
| Repeated R1 stability | `ctest -R gentl_backend --repeat until-fail:20` (GCC and ASan) | 19 tests × 20 passed in each lane |

The 93 tests in each GenTL lane include:

* `gentl_backend_contract`, with the 3 new R1 contract tests;
* 16 isolated `gentl_backend_failure_*` processes, of which 10 are new and 3 are extended;
* the existing GENTL-001 `gentl_consumer_contract`, its 39 R1 cases and `gentl_probe_fixture`;
* `gentl_backend_example_fixture` and `gentl_backend_example_profile_refusal`;
* the unchanged camera-profile suites: `camera_profile_contract`, `device_feature_contract`,
  `cognex_profile_contract` and `dynamic_profile_module_contract`.

Additional checks on the same source:

* `BUILD_TESTING=OFF` with the backend ON builds, and `nm` finds 0 `*_for_testing` symbols in
  `libgige_mel_gentl_backend.a`. `fail_allocation_for_testing` is therefore absent from
  non-test builds.
* `gentl_default_isolation` and `package_isolation_verifier` passed in both the default and the
  backend-ON builds. In the backend-ON build, `libgige_mel_provider.so` has 0 GenTL/Aravis
  dynamic symbols, 0 Aravis/GLib/libxml2 `NEEDED` entries, and no `.cti` or `GCInitLib` strings.
* `python3 scripts/vendor_upstream.py --verify`: 97 pinned files verified.
* `scripts/check_proprietary.py --history origin/main`: 381 Git entries, no prohibited material.

The pre-commit working tree also passed the ASan, TSan and Clang 19 GenTL lanes, with 60/60
`gentl`-labelled tests each. Hosted CI runs only the default build. It does not qualify the
optional GenTL backend, which is established only by the local lanes above.

## C. GenTL backend tests (`gentl_backend_contract` + isolated processes)

The R1 correction extends this suite to 22 contract tests and 16 isolated failure processes
(§C.1).

| Test | What it proves |
| --- | --- |
| `construction_performs_no_io` | No load and no producer call at construction; a null profile throws, so there is no default fallback |
| `composite_identifiers` | Round-trip for IDs containing `:`, NUL or prefix-like text; `ab`+`cd` ≠ `a`+`bcd`; malformed forms rejected |
| `enumerate_and_exact_selection` | Exact, sorted composite IDs. Serial, raw ID, prefix, case-fold and wrong-interface IDs never reach `IFOpenDevice` |
| `mono_default_profile` | The unchanged default profile opens with `DEVICE_ACCESS_READONLY` (2). Capabilities come from native bounds and `<pInc>`, readback equals the register image, 0 `GCWritePort`, close is idempotent |
| `cognex_matching_identity` | The unchanged Cognex module opens in **both** BE and LE register images: 10 Hz envelope, readback `2592x1944 10 Hz 35 ms` |
| `cognex_identity_mismatch` | Wrong vendor, model or firmware is each rejected, naming the feature. No ROI read happens before identity; one `DevClose` |
| `cognex_required_features_and_types` | Each of these fails with no writes: missing TriggerMode, AcquisitionMode or SensorWidth; an Integer-typed rate enable; the wrong rate alias; disabled rate; TriggerMode On; SingleFrame |
| `cognex_bounds_and_readback` | Width `[64, 2592−OffsetX]` with live step 8. Rate `[0.0149, 1e6]` Hz with no invented increment; units Hz/us. Sub-ns exposure is reported, not rounded. 16 Hz reads back unclamped; readback is fresh after a native change |
| `register_endianness` | Raw bytes follow the declared BE/LE order and decode to the same value. Opposite-order bytes decode exactly to `0x00050000`, proving there is no second swap |
| `native_read_failure_propagation` | An injected `GC_ERR_TIMEOUT` returns `timeout` with GenTL name, text and feature. One attempt only; reads recover once cleared |
| `xml_location_handling` | File: and http: are skipped with reasons, and a valid Local location wins. Refused: empty list, ZIP, malformed URLs, 32-bit window, non-.xml name, size limit (no read issued). An out-of-port range gives `GC_ERR_INVALID_ADDRESS` |
| `xml_content_validation` | Refused: ZIP signature, embedded NUL, non-XML, truncated XML, non-GenICam root. NUL padding is accepted. A SHA-1 mismatch is refused; a match is recorded as verified |
| `undeclared_port_byte_order` | `none` and `both` are refused before any `GCReadPort` |
| `unsupported_operations_and_readonly` | `apply()` and `create_stream()` are unsupported. A read-only write is refused before `GCWritePort`. After close: `stopped`, no producer calls, no live GenApi objects |
| `offline_feature_writes` | Explicit control-access fixture session (`access=3`). Ordered coupled ROI writes; the rate is written as a 200000 µs BE period. Live max, read-only, type and absence refusals happen before I/O. An uncertain `GCWritePort` is reported once and not retried |
| `ownership_and_teardown_order` | Repeated 5 times: one live ArvDevice/ArvGc; a second open is busy. The producer stays mapped after the backend is gone. Order is `DevClose`<`IFClose`<`TLClose` with no order violations; the last owner triggers `GCCloseLib` and unmap |
| `unbound_port_access_refused` | An ArvDevice read outside the transaction, or from another thread during one, returns `NOT_CONNECTED`: no `GCReadPort`, no deadlock. A nested transaction gives `invalid_state`. Release frees ArvGc then ArvDevice, then `stopped` |
| `concurrent_transactions` | 4 threads × 25 iterations: no overlap and all succeed. Same-thread reentrant `configuration()` and `close()` give `invalid_state` |
| `profile_portability` | The loaded module still declares only the Aravis and Pleora bits and is injected unchanged |
| `failure_xml_read_failure` | `GC_ERR_IO` mid-XML: partial data discarded, one `DevClose`, no GenApi objects |
| `failure_genapi_creation_failure` | Malformed XML gives `io_error` "not well-formed XML", with cleanup |
| `failure_profile_admission_failure` | An identity-read timeout is propagated with GenTL provenance, with cleanup |
| `failure_devclose_failure`, `failure_ifclose_failure` | Close is attempted once; the error is reported idempotently with no destructor retry; the chain is quarantined and the producer stays mapped |
| `failure_gcclose_failure` | GENTL-001 producer retention: `retained_producers() == 1` |
| `gentl_backend_example_*` | The uninstalled example enumerates. The unchanged default profile refuses the minimal GENTL-001 XML (`SensorWidth`), with no fallback |

These existing tests pass unchanged with the extended fixture:

* GENTL-001: `gentl_consumer_contract`, its 39 R1 allocation/teardown cases and `gentl_probe_fixture`.
* Camera profiles: `camera_profile_contract`, `device_feature_contract`,
  `cognex_profile_contract` and `dynamic_profile_module_contract`.

## C.1 GIGE-MEL-GENTL-002-R1 regression tests

These tests accompany the three corrections in [design §7.1](GIGE-MEL-GENTL-002-design.md). Each
`failure_*` case runs in its own process. Every check counts the fixture's `attempt <close>`
log lines, so "exactly one native close attempt per failed handle" is measured directly rather
than inferred.

| Test | What it proves |
| --- | --- |
| `enumeration_cleanup_control` (contract) | Three repeated enumerations give exact, sorted IDs, with one `IFClose` before one `TLClose` each time. A discovery error whose cleanup succeeds is reported unchanged, with no quarantine, and the producer stays usable. The final release gives `GCCloseLib` and unmap |
| `ownership_reservation` (contract) | A second open while open is `busy` with zero native calls. Explicit close, and destruction while open, both release the identity, so it reopens 3 times. Device B is never blocked by the registry. A conclusive native refusal quarantines nothing |
| `allocation_failure_recovery` (contract) | `native_chain`, `retirement_node` and `registry_entry` (open) and `native_chain`, `retirement_node` (enumerate) each give `resource_exhausted`, zero native calls and nothing reserved. `inspection` and `device_object` each clean up exactly once and conclusively. Each seam fires once. Control: a normal open/readback/close/destroy makes exactly one `DevClose` |
| `failure_enum_ifclose_failure` | Successful enumeration, then `IFClose` fails: 1 attempt, `TLClose` never attempted (the child is unresolved) |
| `failure_enum_tlclose_failure` | `IFClose` succeeds, then `TLClose` fails: 1 attempt each |
| `failure_enum_discovery_ifclose_failure` | `IFUpdateDeviceList` error, then `IFClose` failure. The message starts with the first failure and appends `; cleanup: GenTL IFClose …`. `TLClose` is not attempted |
| `failure_enum_discovery_tlclose_failure` | `IFUpdateDeviceList` error, then `TLClose` failure: first failure kept, cleanup appended, 1 attempt each |
| `failure_enum_system_discovery_tlclose_failure` | `TLUpdateInterfaceList` error, then `TLClose` failure: the system-only path, with 1 `TLClose` attempt |
| `failure_devclose_failure`, `failure_ifclose_failure`, `failure_tlclose_failure` | Extended. After the public device is destroyed, the identity (and device B) is `busy` with zero native calls, so there is no second `IFOpenDevice`. No parent is closed under an unresolved child. Destroying the backend causes no retry and no unmap. A **new backend** instance is refused by GENTL-001 producer retention, with no native call |
| `failure_destructor_close_failure_after_backend` | The backend is destroyed first. The device destructor makes the single `DevClose` attempt and records the tombstone in the registry the backend created (ASan checks the lifetime) |
| `failure_partial_open_cleanup_failure` | Profile admission times out, then the partial-open `DevClose` fails. `timeout` is kept with the cleanup appended. The identity is blocked |
| `failure_consumer_open_cleanup_failure` | `DevGetPort` fails, then the consumer's own `DevClose` cleanup fails (the producer is tainted). `IFClose`/`TLClose` are never attempted. Later operations are refused |
| `failure_allocation_cleanup_failure` | A `device_object` allocation failure, then `DevClose` fails. `resource_exhausted` is kept, the cleanup failure and the unresolved ownership are both in the message, and the identity is blocked |

Every failure case ends with the shared `expect_blocked` check:

* `open()` and `enumerate()` are `busy`, with zero native calls;
* destroying the backend causes no destructor retry, and the library stays mapped;
* `quarantined_native_sessions() == 1`;
* a fresh backend cannot reacquire the producer.

**Mutation check (working tree, GCC lane).** Each mutation below was applied alone, rebuilt and
run against the backend tests. Every one was detected, and the source was restored byte-identical:

| Mutation | Detected by |
| --- | --- |
| Tombstone and producer block not recorded (the old weak-pointer behaviour) | 10 failure cases |
| A failed close drops the members instead of retiring them (destructor retry) | 12 failure cases |
| A parent closed under a child the consumer failed to close | `failure_consumer_open_cleanup_failure` |
| The cleanup diagnostic dropped from the first error | 6 failure cases |

## D. Defect found and mitigated during implementation

`xml_content_validation` failed at first. Aravis 0.8.36 `arv_gc_new` accepted a
truncated document: libxml2 2.9.14 takes the `xmlSAXUserParseMemory` path,
whose positive error results Aravis ignores. The symptom appeared later as
"`[DeviceVendorName] Port not found for node`" plus a GLib CRITICAL. The fix is
an independent libxml2 well-formedness check before `arv_gc_new` (design §4.1).
Aravis is not modified.

## E. Not established

* No real GenTL producer (Gigelink, Balluff or another) has run this backend.
* No physical CIC-5000R has been contacted through GenTL.
* The fixture's XML, register layout and values are simulated. They are not
  the camera's 1,108,591-byte XML.
* The R1 ownership corrections are verified only against fixture-injected failures. It is not
  established how a real producer behaves after a failed `DevClose`, `IFClose` or `TLClose`.
  Any recovery from an unresolved native chain is also unestablished: by design the producer
  stays blocked for the rest of the process.
* Allocation failures are injected deterministically at named points. Real out-of-memory
  conditions inside the producer, Aravis or GLib were not exercised.
* These remain GENTL-003 qualification items (design §10):
  * real producer behaviour for port byte order, read-only access, URL form,
    ZIP and SHA-1;
  * concurrent port calls;
  * call duration.
