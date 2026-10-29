# ADR 0002: Copy into registered host buffers before qualifying direct acquisition

**Status:** Accepted for the bootstrap.

## Context

MEL allows the consumer to initialize and register host buffers, and requires explicit release of a delivered buffer before reuse. Camera SDKs have their own pools, acquisition-result ownership, alignment, and stream-destruction constraints. A retained SDK-owned frame is not automatically a frame in a consumer-registered MEL allocation.

The first implementation must establish reliable ownership and support multiple SDKs. The initial image profile is single-band Mono8 with tightly packed published rows. It does not need a zero-copy transport claim to prove MEL interoperability.

## Decision

Use a move-only backend frame lease to retain source pixels. Copy or repack validated image bytes into an available registered MEL host buffer. Publish the MEL buffer and keep that destination unavailable until the consumer explicitly calls `release()`.

Keep source-pool and destination-pool capacities bounded and independent. Never overwrite a held delivery, hide pool exhaustion by allocating an unbounded queue, or free consumer-owned memory. Preserve source layout, completion status, timestamp meaning, and configuration generation long enough to create a correct MEL header.

Treat acquisition directly into registered MEL memory as an optional future path. It requires a distinct negotiation/qualification step covering at least payload capacity, alignment, minimum buffer count, stride, image offset, chunk trailers, writable registration period, frame cancellation, and native stream/device lifetime.

## Consequences

- Every real SDK can initially use its normal supported acquisition path.
- Row padding can be removed without changing the initial client profile.
- Once a source image has been copied, backend storage can be returned independently of the Skill's MEL lease.
- A consumer bridge can still borrow the already-registered MEL buffer without an additional downstream copy. That does not make the whole pipeline zero-copy.
- The copy consumes memory bandwidth. Measure it on representative hardware before adding lifetime complexity to remove it.
- Direct acquisition needs its own tests for stop, disconnect, reconfiguration, cancellation, and final release. The copy path remains a supported fallback.

## Evidence

The upstream [Channel](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/Channel.h) and [Buffer](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/Buffer.h) headers define registration and explicit release. SDK qualification should use the selected version's primary documentation, for example the Aravis [stream](https://aravisproject.github.io/docs/aravis-0.8/ArvStream.html) and [buffer](https://aravisproject.github.io/docs/aravis-0.8/ArvBuffer.html) contracts or Basler's [pylon programming guide](https://docs.baslerweb.com/pylonapi/cpp/pylon_programmingguide). These are candidate references, not a statement that either SDK is implemented here.
