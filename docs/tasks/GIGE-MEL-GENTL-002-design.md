# GIGE-MEL-GENTL-002 — Generic GenTL backend foundation (design)

Base: `main` `c6d555d4e841eb8ed576615d7716cb54bfebfb28` (merge of PR #39,
GIGE-MEL-GENTL-001). Branch `feature/gige-mel-gentl-002-backend-foundation`.
The final commit/tree and the executed matrix are recorded on the PR and in
the [validation record](GIGE-MEL-GENTL-002-validation.md).

**Scope:** an internal, default-OFF `gentl` `ICameraBackend` that enumerates,
opens, inspects and closes GenTL devices, runs Aravis 0.8.36 GenApi over the
GenTL remote-device port, and admits the **unchanged** camera profiles. It does
**not** do image acquisition, configuration writes through `apply()`, timestamps,
NavigationReport transmission, or any physical-camera or vendor-producer
qualification. Nothing here claims real-camera GenTL operation.

## 1. Pre-implementation audit

I reviewed these files on the fetched `main`: `AGENTS.md`, `include/gige_mel/backend.hpp`,
`src/gentl/gentl_consumer.{hpp,cpp}`, `src/device_features.hpp`,
`src/device_registers.hpp`, `src/profiles/camera_profile.hpp`,
`src/profiles/mono_free_running*`, the Aravis (`AravisDeviceFeatures`) and
Pleora (`pleora_features.cpp`) feature adapters, `src/selection/*`,
`camera_profiles/cognex_cic_5000r/*`, `docs/camera-profile-portability.md` and
both GENTL-001 records.

The audit established this dependency chain:

| Layer | Requires | Contract used |
| --- | --- | --- |
| `ICameraProfile::open(IDeviceFeatures&)` | a typed, synchronous feature service for one transaction | identity, `implemented`, typed reads with fresh bounds/increments/units; never retains the service |
| `IDeviceFeatures` (GenTL) | a live `ArvGc` node map **and** exclusive use of the port during the call | Aravis node semantics identical to `AravisDeviceFeatures` |
| `ArvGc` (Aravis 0.8.36) | an `ArvDevice` whose `read_memory`/`write_memory` reach the camera | `arv_gc_new(device, xml, size)` stores the device **borrowed** |
| GenTL-backed `ArvDevice` | `gentl::Device::read_port` / `with_registers` | `GCReadPort`/`GCWritePort` under the consumer device mutex, declared port byte order |
| `gentl::Device` | `Interface` → `System` → `Producer` (children own parents) | GENTL-001 ownership, taint and process-lifetime retention |
| `ICameraDevice` lifecycle | everything above, alive until `close()` | `configuration()` fresh; `close()` idempotent |

**No existing source interface changed.** The profile ABI, `IDeviceFeatures`,
`ICameraBackend`/`ICameraDevice`, the profile module descriptor (no new
compatibility bit), the GenTL consumer and the public factories are untouched.
The fixture producer gains test-only controls (§9).

## 2. GenApi architecture and native API evidence

The chosen implementation is **Aravis 0.8.36 `ArvGc` over a project-owned
`ArvDevice` subclass**, `GigeMelGenTLArvDevice`
(`src/backends/gentl/gentl_genapi.cpp`). The evidence comes from the pinned release
archive (`aravis-0.8.36.tar.xz`, SHA-256 `246deaa0…51bf`, tag commit `97ff5e33`),
which `scripts/provision_aravis.sh` verifies:

* `arvdevice.h` declares `ArvDevice` as a derivable type. `ArvDeviceClass` has the
  virtuals `create_stream`, `get_genicam_xml`, `get_genicam`, `read_memory`,
  `write_memory`, `read_register` and `write_register`. The type is abstract and
  initable; with no `init_error`, construction with `g_object_new` is valid. All
  seven virtuals are implemented; `create_stream` returns
  `ARV_DEVICE_ERROR_NO_STREAM_CHANNEL`.
* `arvgc.c`: `arv_gc_new` sets `priv->device = device` **without a reference**.
* `arvgcport.c`: the 4-byte `read_register` path and its GigE big-endian
  swap run only when `ARV_IS_GV_DEVICE`. For this subclass, every register
  access is a raw `read_memory`/`write_memory` byte transfer.
* `arvgcregisternode.c`: the node then converts once using its own `<Endianess>`.
* `arvdomparser.c`: the SAX state is function-`static`, so parsing is
  serialized process-wide (`parser_mutex`).
* `arvgcfloat.c`: `G_MINDOUBLE` from `arv_gc_float_get_inc` means "no increment".

The implementation does not use:

* Aravis's experimental GenTL consumer (0.9.x only).
* `ArvGvDevice`, `ArvInterface` or `arv_open_device`, so there is no second GigE Vision connection.
* Aravis 0.9.x or 0.10.
* Modified Aravis sources.

The isolation test checks the source for these entry points.

### Endianness without double conversion

`read_port` moves raw bytes, and `ArvGcRegisterNode` applies the register's
declared `<Endianess>` exactly once. The port's `PORT_INFO_*_ENDIAN` must be
declared and unambiguous. Otherwise GenApi is refused before any XML read. That
order is used only for direct `read_register`/`write_register` callers, through
the GENTL-001 `CheckedRegisters` transport.

Aravis's GigE schema-1.0 legacy heuristic does not apply to this class. The
schema version is recorded (`XmlFacts::legacy_schema`) for comparison during
producer qualification. The real Cognex XML is schema 1.0.1 with explicit
per-register `<Endianess>`. Whether a real producer presents those bytes
unchanged is a GENTL-003 question (§10).

### Lifetime and ownership

```
GenTLDevice ─ NativeChain ─ GenApiSession ─ Native ─ GigeMelGenTLArvDevice ─ ArvGc
                         └─ gentl::Device ─ Interface ─ System ─ Producer (dlopen)
```

* The `ArvGc` has exactly one owner, its `ArvDevice`. `vm_finalize` unrefs it
  **before** chaining to the parent finalizer, and a weak reference clears the
  pointer. A surviving external reference would be an ordering violation. It is
  detached and counted, and tests assert that count is zero.
* `GenApiSession::release()` destroys the ArvGc and then the ArvDevice with no native I/O.
  Only then does `NativeChain::close()` run `DevClose`, `IFClose` and `TLClose`.
  Every child owns the producer, so it outlives every GenApi object and port call.
* Release is refused from inside the owning transaction thread. Transactions
  after release return `stopped` and never touch the node map.

## 3. Concurrency and reentrancy

* Lock order is always `GenTLDevice::mutex_` → `GenApiSession::mutex_` →
  `gentl::Device::mutex_`. The last is a **leaf**, taken only for the duration of
  one `GCReadPort`/`GCWritePort` (`read_port`, `with_registers`). No callback,
  profile code or Aravis code runs under it, so nested device-lock acquisition
  cannot happen.
* One complete profile transaction (snapshot, plan, ordered writes, readback)
  runs under the first two locks. The profile's `ConnectionCheck` reads only
  local state.
* For the duration of one transaction, the `ArvDevice` holds an atomic pointer to a
  stack-local `Binding` that is bound to the owner thread. A virtual-method call
  without that binding is refused with `ARV_DEVICE_ERROR_NOT_CONNECTED` and no
  native call. This covers stray Aravis callers, other threads and calls after
  close.
* Same-thread reentry into `configuration()`, `close()` or a nested transaction
  returns `invalid_state` instead of self-deadlocking.
* No new producer worker or background thread is created.

## 4. GenICam XML handling

At explicit `open()`, after `IFOpenDevice`/`DevGetPort`, in this order:

1. **Port byte order** must be declared and unambiguous; otherwise the device is
   refused (`unsupported`) before any read.
2. **Locations** come from `GCGetNumPortURLs`/`GCGetPortURLInfo` (≤ 64). The
   lowest-index location that passes every check is chosen:
   * The URL begins with `Local:` (case-insensitive).
   * `URL_INFO_SCHEME`, if reported, is `URL_SCHEME_LOCAL`.
   * The URL has the form `name;hexaddr;hexlen` and the length is non-zero.
   * The file name ends in `.xml`. A `.zip` name is unsupported.
   * The length is at most `max_xml_bytes` (default 16 MiB; the real Cognex XML is 1,108,591 bytes).
   * The address range lies in the 32-bit register window.
   * `URL_INFO_FILE_REGISTER_ADDRESS`/`FILE_SIZE`, when reported, agree with the URL.

   Every rejected location is recorded with its reason. `File:` (host files)
   and `http:` (network) are explicitly unsupported. If nothing qualifies, open
   fails `unsupported` and lists every reason.
3. **Retrieval** reads the full declared length in 64 KiB chunks. Allocation
   happens only after the limit check. Any failure or short transfer discards the
   whole document and keeps the exact GenTL error.
4. **SHA-1**: if the producer reports `URL_INFO_FILE_SHA1_HASH`, it is
   verified over the exact bytes, and a mismatch is an `io_error`. Absence is
   recorded as "not verified".
5. **Content checks**:
   * A ZIP signature is refused.
   * Only trailing NUL padding is removed. An embedded NUL, or content that does
     not start with `<` (after an optional BOM and whitespace), is refused.
   * The document must be well-formed and rooted at `RegisterDescription`
     (§4.1).
6. **Construction**: `arv_gc_new` under the parser lock, then a check that
   the root is an `ArvGcRegisterDescriptionNode`. The per-session policies are
   cache **disabled**, range check **enabled** and access check **enabled**.

### 4.1 Demonstrated Aravis limitation and mitigation

On this host's libxml2 2.9.14 (< 2.11), Aravis 0.8.36 parses with
`xmlSAXUserParseMemory` and treats only *negative* results as errors. A
truncated document therefore produced a partial `ArvGc`, and the first
symptom was a later "Port not found" GenApi error (observed by
`xml_content_validation` before mitigation). The backend now runs an
independent libxml2 parse with `XML_PARSE_NONET` and no entity loading.
libxml2 is already Aravis's own private parser dependency. Malformed XML is
reported as `io_error: not well-formed XML: <libxml2 message> (line N)`.
Aravis itself is not modified.

### 4.2 Compressed XML (ZIP)

Not supported in this task. All checked forms of a compressed device file return
an explicit `unsupported`: a `.zip` URL, or a ZIP signature behind a `.xml` name.
Aravis 0.8.36 exports `arv_zip_new`/`arv_zip_get_file`, but in its source:

* It trusts unbounded central-directory sizes and offsets from the archive.
* `inflate` results are unchecked, and `uncompressed_size` is allocated
  directly from the header.
* It supports only stored/deflate entries.

Bounded support therefore needs its own validated reader. That reader must:

* Check every offset against the buffer, cap entry count and sizes, and
  require exactly one `.xml` entry.
* Check `inflate` return codes and cap decompressed bytes at `max_xml_bytes`.
* Verify the CRC-32.
* Use zlib, which is already an Aravis dependency.

Because this is not straightforward, it is deferred, not partially implemented.
The real CIC-5000R's URL form is unknown until GENTL-003.


## 5. Feature-access mapping (`IDeviceFeatures` → Aravis node API)

These are the same calls and semantics as the native Aravis adapter, so one
profile sees identical facts through either path.

| Operation | Native mapping | Distinctions preserved |
| --- | --- | --- |
| `implemented` | `arv_gc_get_node` + `arv_gc_feature_node_is_implemented` | absent → `false`; non-feature node → error; query failure → error (never absence) |
| precondition for every read and write | `is_implemented`, `is_available`, `get_actual_access_mode`; for writes also `is_locked` | absent, unimplemented, unavailable, inaccessible (RO/WO/NA/NI by name) and locked are separate `unsupported` diagnostics |
| `read_integer[_feature]` | `ARV_IS_GC_INTEGER`; `get_value/min/max/inc` | wrong type is `unsupported`; live bounds are not clamped |
| `read_float_feature` | `ARV_IS_GC_FLOAT`; `get_value/min/max/inc/unit` | `G_MINDOUBLE` → no increment; any other value is kept for the profile to judge |
| `read_string_or_enum` | enumeration current entry, or string value | other node types are `unsupported` |
| `read_boolean` | `ARV_IS_GC_BOOLEAN` | e.g. an `Integer` named `AcquisitionFrameRateEnable` is rejected |
| `enum_entry_available` | `arv_gc_enumeration_dup_available_string_values` | the current entry must be available |
| `write_integer` / `write_float` | `arv_gc_{integer,float}_set_value` (range and access policy ENABLE) | one native write; live max, increment, access and type rechecked at the write; no clamp, retry or rollback |

**Error provenance.** A port failure records the exact GenTL failure
(`GC_ERROR` name and number, `GCGetLastError` text, function, mapped
`ErrorCode`) in the transaction binding. The feature layer returns that
failure instead of Aravis's generic transfer GError. For example, an injected
`GC_ERR_TIMEOUT` returns `ErrorCode::timeout` together with the producer text.
A write failure after `GCWritePort` was entered says
`write completion uncertain; not retried`.

**Mutable values** are never cached (cache policy disabled; every read goes to
the port). The profile state retains only its open-time envelope, exactly as
with Aravis and Pleora.

## 6. Backend and device contract

* `id()` is `gentl`. `make_backend(Options, profile)` performs no I/O and
  rejects a null profile; there is no default-profile fallback.
* **Composite device ID:** `gentl1:<n>:<interface-id><device-id>`, where `<n>`
  is the decimal byte length of the interface ID with no leading zeros. It is
  length-encoded rather than delimiter-escaped, so it is reversible for any bytes.
  Parsing rejects a wrong prefix, a malformed or zero length, and an empty
  device ID.
* **`enumerate()`:**
  1. Load the producer from the exact absolute path, on first explicit use.
  2. `TLUpdateInterfaceList`, then list the interfaces.
  3. For each interface: `IFUpdateDeviceList` and the device list.
  4. Close the interfaces and the system.

  This produces stable, sorted, exact IDs. Display text is informational only
  and never used for matching.
* **`open(id)`:**
  1. Parse the ID.
  2. Busy if the same ID is already open through this backend.
  3. `TLOpen`, then `TLOpenInterface` for the exact interface.
  4. `IFUpdateDeviceList`, then `IFOpenDevice` with the exact device ID and the **configured**
     access: `DEVICE_ACCESS_READONLY` by default, `CONTROL` only when
     explicitly configured. Access is never escalated.
  5. Construct the GenApi session (§4).
  6. Run the injected profile's `open()` in one transaction.

  Any failure closes what was opened, once, in reverse order. The selected
  profile alone decides compatibility.
* **`ICameraDevice`:**
  * `capabilities()` returns the accepted profile state's capabilities.
  * `configuration()` is a fresh read-only readback through the state.
  * `apply()` and `create_stream()` return `unsupported` without any
    native call.
  * `close()` releases GenApi, then `DevClose`, `IFClose`, `TLClose`. It is
    idempotent and returns the first result again.

  A read-only session refuses writes before `GCWritePort` even through the
  internal feature service.
* **Offline write tests** use `with_features_for_testing`, which is compiled
  only with `GIGE_MEL_GENTL_TESTING` (fixture builds) and drives the
  `IDeviceFeatures` service of a control-access fixture session. The public
  `apply()` path stays disabled.

## 7. Native teardown and quarantine

`NativeChain::close()` attempts each native close at most once:

* `DevClose` is attempted.
* If `DevClose` fails, the interface and system are left open.
* `IFClose` and `TLClose` are attempted only if their child closed.

The first failure is reported with later diagnostics appended. On any failure,
the whole chain, including the producer it owns, moves into a deliberately
leaked process-lifetime list (`quarantined_native_sessions()`). No destructor
can retry a close, and the library is never unmapped under unresolved native
state. A failing `GCCloseLib` keeps the existing GENTL-001 producer retention
(`retained_producers()`). Tests cover failures of `DevClose`, `IFClose` and
`GCCloseLib`, each in its own process.

## 8. Build isolation

* `GIGE_MEL_BUILD_GENTL_BACKEND` is OFF by default and requires
  `GIGE_MEL_BUILD_GENTL_PROBE` (the verified GenTL header and consumer) plus
  exactly `aravis-0.8 = 0.8.36` through pkg-config.
* Static, internal target `gige_mel_gentl_backend`: not installed or exported,
  not linked into `gige_mel_provider`, absent from `make_builtin_catalog` and
  the public MEL factories, and with no new profile-module compatibility bit.
* Nothing is downloaded during configure or build.
* The uninstalled `examples/gentl_backend_fixture.cpp` shows explicit
  construction and read-only use.
* `tests/gentl_isolation_tests.py` guards all of the above in the default build.


## 9. Simulated fixture and camera-profile execution

**Producer.** The GENTL-001 fixture producer (`tests/support/gentl_mock_producer.cpp`)
gains test-only controls. Its default behaviour is unchanged, and the GENTL-001
tests still pass. The controls:

* replace the port image,
* set an explicit URL list,
* set a producer SHA-1,
* inject a `GCReadPort` failure at an address,
* peek/poke port bytes.

**Register model.** `tests/support/gentl_genicam_fixture.hpp` is a **simulated**
GenICam register model. It is an original test arrangement, not vendor XML or
physical evidence. It reproduces only:

* the documented CIC-5000R identity strings;
* the node **types** the profile reads, with explicit bounds/units:
  * `StringReg` identity;
  * `IntReg` sensor size;
  * `Integer` ROI with live `IntSwissKnife` coupled maxima and a register-driven
    `<pInc>`;
  * `Enumeration` PixelFormat, TriggerSelector, TriggerMode, AcquisitionMode and
    ExposureAuto;
  * `Boolean` `AcquisitionFrameRateEnable`;
  * `AcquisitionFrameRateAbs` in Hz through a non-linear `Converter` over an
    integer µs period register;
  * `ExposureTimeAbs` in µs over a `FloatReg`.
* every register in either big- or little-endian `<Endianess>`.

Executed through the real backend, the real Aravis 0.8.36 GenApi and real
`GCReadPort`/`GCWritePort`:

| Profile | Result (fixture only) |
| --- | --- |
| `mono_free_running_v1` (built-in, unchanged) | opens; capabilities from native bounds; configuration readback matches the register image |
| `cognex_cic_5000r_v1` (unchanged dynamically loaded module) | opens on the matching identity in both register byte orders. These are rejected with the feature named and no writes: vendor/model/firmware mismatch; missing TriggerMode/AcquisitionMode/SensorWidth; Integer-typed rate enable; wrong rate feature name; disabled rate; trigger on; SingleFrame |

The same module is unchanged. It still declares only its Aravis and Pleora
compatibility bits. GenTL composition is explicit C++ injection, as the
GENTL-001 design anticipated (E3 deferred to GENTL-005).

## 10. Missing functionality, risks and future qualification

* **Unsupported here:**
  * Configuration `apply()` and streaming.
  * Timestamps; `INavigationSink` remains unimplemented.
  * ZIP, `File:` and `http:` XML.
  * Selection catalog entry and module compatibility bit.
  * Chunk and event ports (Aravis reports them unsupported).
* **Producer compatibility risks (unqualified):**
  * The real port's byte order and whether register bytes reach the port
    unmodified (the schema-1.0 legacy question).
  * The actual URL form, ZIP use and SHA-1 presence.
  * Producer behaviour on `DEVICE_ACCESS_READONLY` (some producers refuse
    reads or GenApi features in that mode).
  * Producer thread safety for concurrent `GCReadPort`.
  * Port call duration bounds.
  * GENTL-001 found two producers that crashed on legal call sequences
    (condition C3).
* **Read-only versus control access:**
  * Read-only is sufficient for open, identity, profile admission and readback.
  * Control access is required for any future `apply()` or acquisition.
  * It must be configured explicitly and is never escalated.
* **GENTL-003 hardware qualification needs:**
  1. A separately authorized read-only open/inspect/close of the CIC-5000R
     through a qualified producer (e.g. Euresys Gigelink).
  2. Comparison of identity, schema, port byte order, XML SHA-1/length and
     feature readback against the recorded Aravis/Pleora evidence.
  3. No writes.
  4. Acquisition only in a later, separately authorized task.

