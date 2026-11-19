# Source-backed stacked-image foundation

GIGE-MEL-044 implements a common-provider `StackedImageChannel` for explicitly
qualified, pre-formed test products. It does **not** qualify TSI processing,
physical cameras, sensor defects, optical alignment, timestamps or complete MEL
conformance. Synthetic, Aravis, Pleora and GenTL devices have no stack source and
remain stack-unsupported and unadvertised. No production adapter was changed.

## Published contract and profile decision

At pinned IR MEL `8d9224519f12b44e0b28815755c56a32a28d24a0`, the channel
class is `@Required`; `StackedImageListener` and `onImageStack` are
`@RequiredIfImageStack`. This does not make the entire class optional or make
an unsupported stub conformant. This experimental profile implements the class
and inherited surface, but discovers it only for an explicitly qualified source.
`Config::getImgLstnr()` returns `shared_ptr<ImageListener>`;
`StackedImageListener` derives from it. Attachment requires a successful
`dynamic_pointer_cast`, the exact advertised stack identity and `StackedImage`
type, and permits one attached stack channel per manager. Existing Image/C2/Health
UUID suffixes 1/2/3 are unchanged; Stack uses 4 and `/stacked-image`.
No alternative `IRFactory`/registry entry point is exported.

```cpp
Return onImageStack(Channel&, const StackFrameHeader&, std::shared_ptr<Buffer>);
```

Unlike Image's void callback, this result is observed. `Success` continues;
`Fail`, `NotSupported`, any other non-success, or an exception terminally fails
the owned acquisition and cancels receive. Delivered/callback-failure statistics
and diagnostic text record the actual outcome. Repeated enable reports failure
until disable cleans up. **No callback result releases a delivered buffer.**
Even a throwing listener may already have retained its buffer.

## Source contract and exact layout

The optional project-owned `IStackedImageSource`, implemented alongside
`ICameraDevice`, has no MEL or SDK types. Existing device vtables are unchanged.
`StackSupport` is immutable for the device lifetime. It declares source ID,
nonzero source generation, capability/activity UUIDs and labels, an explicit
identity scheme, stack formation meaning, frame dimensions, fixed supported
stack depth, layout, packed byte count, platform ID, TXA FOM and its meaning,
and explicit absence of mask information. Incomplete declarations are not
advertised. Revalidation at enable and exact descriptor comparison at receive
fail closed on changing declarations.

Only contiguous Mono8, frame-major then row-major, unsigned 8-bit storage and
8 significant bits, depth greater than one is accepted. The complete byte count
is `width * height * depth`; each multiplication is checked against `size_t`
before execution and the result must fit `uint32_t` and equal the declared size.
Width, height and depth must fit `uint16_t`; storage/logical depths must be 8
(therefore also fit `uint8_t`); platform ID fits `uint16_t`; FOM is 1–10.
No multipart, padding, compression, Mono12, GPU, RDMA or multiband support exists.

The test fixture supplies two pre-rendered intensity planes of one test volume,
4 by 3 pixels each: bytes 1–12 and 101–112. These are **not camera frames** and
are not asserted to be production TSI. The provider does not form, concatenate
or interpret stacks. Stack depth is a layer/frame count, not spectral bands.
Capabilities report width 4, height 3, bit depth 8, row pitch 4, one Mono band,
image size and required buffer size 24, and only communications metadata.
`ChannelCapability` has no separate stack-depth or storage-depth field; those
are established by the profile and delivered header, not invented metadata.

### Upstream ambiguity

`packedPixelsSize` is described as total packed data size but its comment gives
`bitDepth X height X width X stackDepth`, which computes bits. Units are not
explicitly specified. **This local profile uses bytes**, agreeing with buffer
capacity and copy length, i.e. divides that expression by eight. Consumer
agreement is required; this is not a claim that upstream settled the ambiguity.
The term TSI does not define formation or layer interpretation sufficiently to
qualify a physical product. This implementation accepts only the declared test
volume meaning; deployment-specific TSI qualification remains open.

## Exact header mapping

| Upstream field | Source-backed value and validation |
| --- | --- |
| `platformId` | Declared platform ID, checked `uint16_t` |
| `txaFOM` | Declared rank and documented meaning; upstream 1(best)–10 |
| `stackDepth` | Declared depth, 2–65535, checked before narrowing |
| `capabilityId`, `activityId` | Declared UUID and label, not generated mission IDs |
| `stackId` | Source record key under declared scheme, `uint32_t`; no provider counter |
| `packedPixelsSize` | Checked complete Mono8 stack **bytes**, `uint32_t` |
| `height`, `width` | Declared frame dimensions, 1–65535 (private fields are `uint16_t` despite wider accessors) |
| `bitDepth`, `pixelDepth` | Storage/logical depths, both 8, checked before `uint8_t` casts |
| `deadPixelMask` | Zero only with declared absence of information; no defect characterization |

The constructor takes **height before width**. It is used only after validation
to avoid the narrowing setters. There is no integration-start time field in
`StackFrameHeader`; the provider invents no timestamp or synchronization claim.

## Ownership, lifecycle and bounded work

`StackLease` is move-only, containing descriptor, read-only span and an owner
preserving immutable source bytes until the copy finishes. It carries source
and configuration generations. The owner must actually back the entire span;
the provider can validate ownership presence and exact size, not arbitrary C++
pointer provenance. Generation mismatch, unsupported layout, malformed dimensions,
missing owner or truncated payload rejects the stack and fails the stream.
Source errors terminally fail; timeouts are ordinary polling. Sources must not
invoke consumer/provider callbacks. Open and receive are bounded and cancellation
is concurrent, idempotent, nonblocking and wakes receive promptly.

Enable requires a listener and previously registered host buffers, each covering
the complete packed stack and not outstanding. Only successful validation reaches
source open; no discovery or attachment opens a stream. Failed setup retires
partial resources. One worker copies within validated source and destination
bounds. A full consumer pool drops incoming work and increments no-buffer drops;
there is no unbounded queue or overwrite. Source implementations must bound their
own pending work; the fixture holds one pending product.

The existing HostBuffer available/filling/delivered states are reused. Delivered
bytes remain occupied until explicit `Buffer::release()`, which is safe inside
the callback. Disable and detach never release or erase consumer data. The
consumer owns the allocation and must keep it alive while registered or held.
The manager tracks weak buffer reservations, preventing overlapping memory
registration across Image and Stack, including consumer-held detached buffers.
Unregister requires stopped acquisition and a released buffer.

Image and Stack exclusively share acquisition ownership; there is no concurrent
source capability in this profile. Configuration is blocked during either
stream. C2 Standby cancels and joins the active acquisition, leaving held memory
and attachment intact. The established software Health/BIT state sees Stack as
owned acquisition: Operate while healthy, acquisition-failed when terminally
failed, and Standby after retirement. This means software acquisition, **not
physical scanning**; scheduling/tasking and physical-state claims are unchanged.

Disable cancels receive and joins. Detach closes admission under the callback
mutex, cancels and joins, then drains metadata reservations. User code executes
outside all operation/state/callback locks. Lifecycle mutations from any provider
callback context fail before locking. A worker's Control/channel/runtime pins
preserve lifetimes when external owners disappear; defensive self-teardown
detaches only its own worker, whose captured state remains alive to retire it.
Completed external teardown admits no late callback. As with existing Image,
teardown cannot forcibly terminate arbitrarily blocking consumer code: callbacks
must return. Source cancellation bounds do not imply a deadline for user code.

See [design](tasks/GIGE-MEL-044-design.md) and
[validation](tasks/GIGE-MEL-044-validation.md) for evidence and limits.