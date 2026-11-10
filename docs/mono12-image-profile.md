# Unpacked Mono12 image profile (GIGE-MEL-022)

## Scope and selection

Supported formats are **Mono8** and **unpacked Mono12**, one monochrome band.
No Mono12Packed, Mono12p, Mono10Packed, Bayer, RGB, multi-band, unpacking,
scaling, normalization, intensity modification or automatic conversion is added.
MEL interfaces and factory signatures are unchanged.

`getAPI_Manager("synthetic")` and `make_synthetic_backend()` remain Mono8.
An embedding explicitly selects the internal synthetic profile:

```cpp
gige_mel::backend::SyntheticOptions options;
options.pixel_format = gige_mel::backend::PixelFormat::mono12;
auto backend = gige_mel::backend::make_synthetic_backend(options);
auto manager = gige_mel::make_api_manager({}, backend);
```

`CameraCapabilities::pixel_format` describes the selected device/profile format,
not a list or command selector. CameraConfiguration, MEL CameraCommand and
ConfigSet do not select it. The legacy internal `mono8` boolean is false for
Mono12. Internal C++ backend value types are **not a stable plugin ABI**;
backend/embedding clients must rebuild when these types change. MEL-only Mono8
clients retain the pinned binary interfaces and default behavior.

## Authoritative representation and byte order

* [EMVA PFNC 2.4](https://www.emva.org/wp-content/uploads/GenICam_PFNC_2_4.pdf),
  §5 and §6.1.1, pp. 30–32, Figure 6-3: unsigned Mono12 is **LSB aligned** in
  a 16-bit storage word; bits 11..0 are significant, bits 15..12 are zero.
  Mono12msb is a different layout and is unsupported.
* **PFNC does not specify word byte order.** §1.4, pp. 10–11, leaves memory
  endianness to the referencing standard, although its figures use little-endian.
  Do not infer it from Figure 6-3, the PFNC code, register byte order or host CPU.
* [EMVA SFNC 2.7.1](https://www.emva.org/wp-content/uploads/GenICam_SFNC_v2_7.pdf),
  §3.46, pp. 105–106: `DeviceStreamChannelEndianness` describes **multi-byte pixel
  data**, with enumeration values `Big` and `Little`. It is distinct from
  DeviceRegistersEndianness and chunk/header encoding.
* Pinned [Aravis 0.8.36 arvenums.h](https://github.com/AravisProject/aravis/blob/97ff5e330919d7a44b871df4eda669feeafdf342/src/arvenums.h)
  defines `ARV_PIXEL_FORMAT_MONO_12 = 0x01100005` (16 occupied bits), distinct
  from `MONO_12_PACKED = 0x010c0006` and `MONO_12P = 0x010c0047`.
  Its `arvgvstream.c` copies GVSP pixel bytes without swapping. Neither this
  adapter nor the provider converts them.

This project's output profile explicitly selects **little-endian** words:
sample `0x0abc` is bytes `bc 0a`. Synthetic generation writes bytes explicitly,
without host-endian casts. Aravis requires native
`DeviceStreamChannelEndianness` readback exactly **`Little`**. `Big`, unknown,
absent or failed readback cannot establish Mono12 and is rejected. Cameras
lacking that feature need independently verified representation provenance and
an explicit profile extension, not guessed bytes. No transport-wide byte-order
assumption or camera-specific fallback is made.

## Pinned MEL mapping

Unchanged upstream ChannelCapability documents dynamic-range bit depth (giving
a 12-bit ADC example) and row pitch in **bytes**. FrameHeader defines stored
bits per pixel and byte-aligned rows. ImageChannel, ImageListener and Buffer
retain registered consumer storage and explicit `Buffer::release()` ownership.

| Field | Mono8 | Mono12 |
| --- | ---: | ---: |
| Significant bits / ChannelCapability.bitDepth | 8 | 12 |
| Storage bits / FrameHeader.bitsPerPixel | 8 | 16 |
| MEL format | Mono | Mono |
| Bands | 1 | 1 |
| rowPitch | width | width × 2 |
| bufferSize, imageSize | width × height | width × height × 2 |
| Byte order | Not applicable | Little-endian, LSB aligned |

64×48 Mono12 requires **6144 bytes**, row pitch **128**. Full 640×480 synthetic
Mono12 requires **614400 bytes**. `image_layout()` checks products before
allocation/narrowing; the provider also requires image bytes to fit MEL's
uint32 fields. Empty, overflowing and impossible geometry is rejected.
One-byte-short registration returns BadPointer; buffers made too small by
later reconfiguration prevent enable.

## Admission, row copy and ownership

Before acquiring or writing a destination, the provider validates selected
format, significant/storage bits, byte order, one band, complete status,
dimensions, actual configuration, configuration/stream generation, stride,
span and the unchanged integration-start timestamp contract. Invalid first
frames cannot establish accepted stream generation. Timestamp epoch change or
regression stops acquisition before copying the offending frame.

For `R = width × 2` and source stride `S >= R`, minimum readable span is
**`(height - 1) × S + R`**, checked without overflow. The provider copies
**exactly R bytes per row**, stripping X padding; it never reads or invents
final-row padding. Aravis accepts only this minimum or `height × S` (actual
final X padding), image-part size equal to received bytes, inside allocation
capacity. Chunk, multipart, incomplete, hidden prefix and inconsistent trailing
bytes are rejected.

Full consumer pools drop incoming frames. Held destination bytes survive stop,
Standby, detach and shutdown until explicit release. Source leases independently
retain immutable storage. Throwing callbacks stop acquisition without
auto-releasing a potentially retained buffer.

## Synthetic and simulated evidence

Synthetic Mono12 uses `(17*x + 31*y + native_frame_id) & 4095` with ROI-local x/y,
low byte then high byte. Mono8 keeps `(x + y + native_frame_id) & 255`.
Tests verify full-frame values/high bytes, successive frames, byte counts,
padded rows with/without final padding, reconfiguration, pool exhaustion,
release, restart, Standby, exceptions, retained bytes and negative metadata,
generation and arithmetic controls.

`aravis_mono12_simulated` uses real Aravis control/stream/buffer ownership with
an **explicitly arranged** Mono12 enum, stream-endianness feature and fill
callback through public SDK APIs. Stock Fake provides neither Mono12 enum nor
generator. The existing decoded-facts seam supplies generated received/image
lengths only (no public setter exists). Pixels `(17*x + 31*y + native_id) & 4095`
are checked across all 3072 samples. This is **simulated adapter qualification**,
not native Fake Mono12 transport or physical-camera evidence. Existing Mono8
Fake and UDP loopback tests remain.

## Aravis and hardware limitations

Open reads PixelFormat and **never sets it**. Manual exposure, TriggerMode Off,
Continuous acquisition, ROI/readback, exact integer-Hz representability, source
pool bounds, control-loss and cancellation rules remain. Format/endianness
changes are rejected by configuration/apply/start or frame admission; explicit
close/reopen is required.

Production timestamps stay **invalid**; the provider rejects them. Positive
simulated or isolated opt-in physical MEL delivery uses only the existing
test-only virtual clock, never a production bypass. The PGM/255 diagnostic
explicitly rejects Mono12 before file creation rather than truncating words or
mislabeling little-endian words as a 16-bit PGM.

**Cognex CIC-5000R physical Mono12 testing is pending owner authorization.**
Advertisement does not prove native endianness/readback or captured pixels.
No physical camera settings were changed for this feature. The opt-in physical
MEL test handles either admitted format but writes no feature. Before a write:
obtain approval, snapshot all original writable settings outside Git, stop
acquisition, select unpacked Mono12 and verify format/endianness and exact
readback at a valid low rate; capture, restore originals, and independently
verify restoration. Missing native endianness remains an unsupported gate.
No UserSet, firmware, IP, MTU, GevSCPD, receive-buffer tuning or persistent changes.

Executed checks are recorded in [validation](validation.md#unpacked-mono12-gige-mel-022).