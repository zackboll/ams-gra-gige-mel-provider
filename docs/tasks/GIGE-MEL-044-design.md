# GIGE-MEL-044 design and pinned-contract audit

Base: `af1f694fa0e1edcc968cfdef945341a17aa5c614`, tree
`269795b3715b53261c6b22f785048529df4e575b`. Branch:
`feature/gige-mel-044-stacked-image-foundation`.

## Contract inspection and boundaries

Reviewed exact pinned `StackedImageChannel.h`, `StackedImageListener.h`,
`StackFrameHeader.h`, `Channel.h`, `ChannelType.h`, `ChannelCapability.h`,
`Config.h`, `Buffer.h` and `IRFactory.h`. The first three were absent from the
initial include closure. They are added byte-for-byte from the **same** pinned
revision, with Git-tree blob identities and SHA-256/size in the manifest.
All existing locked files, original licensing/intent and upstream revision
remain unchanged. The three headers include only already-locked dependencies;
the closure grows from 91 to 94 headers and from 97 to 100 locked files.
`IRFactory` was inspected in an external exact-revision checkout, not installed
or exported as a new path.

Channel class `@Required` is distinct from delivery `@RequiredIfImageStack`.
Inherited common operations use existing ChannelCore; optional LOS3D remains
unsupported. `Config` stores an ImageListener pointer, requiring a dynamic stack
listener check. Callback returns `Return`, unlike Image; non-success is terminal
but never an implicit memory release. Buffer's explicit-release contract governs.

Private header widths, constructor height/width order, storage versus significant
bits, packed-size comment ambiguity, absence of time, source identities and
TXA FOM qualification are resolved explicitly in the
[provider profile](../stacked-image-provider.md). Byte units are a local agreement,
not an upstream or TSI-qualification claim.

## Implementation choices

1. Add an optional, MEL/SDK-independent multiple-inheritance source interface;
   no changes to existing device methods or production adapters.
2. Freeze validated product declarations before discovery; advertise only
   complete contiguous Mono8 products. No provider stack formation.
3. Use owned move-only leases, exact descriptor and generation checks and bounded
   cancellable receive. Source code never enters consumer callbacks.
4. Reuse host buffer states and runtime acquisition publication for Stack.
   Extend weak memory reservations across channels to reject aliased memory,
   including held buffers after detach. Do not allocate destination memory.
5. Share exclusive acquisition and configuration exclusion with Image; C2 Standby
   and software Health/BIT use the same acquisition ownership/failure snapshot.
6. Serialize stack admission against detach, invoke outside locks, and drain by
   worker join plus callback reservations. Preserve held data on every outcome.
7. Keep the recording fixture test-only. Installed consumer's callback translation
   unit uses only MEL headers; a separate application composition translation
   unit supplies the SDK-independent recording source through the existing seam.

No navigation, LOS, ordinary Image payload/timestamp, CameraCommand, camera
profiles, scheduling, factory signatures, SDK behavior or Pleora sanitizer
exceptions are changed. Source lifetime and cancellation requirements are
documented rather than claiming safe behavior from a contract-violating source.

## Qualification strategy

An independent constant oracle checks every header field and destination byte,
including untouched destination tail. A one-pending-product fixture controls
timeouts, failures, malformed descriptors and lengths, identity/generation
changes and cancellation. Receive-entry barriers are captured under the fixture
mutex so timeout polling cannot fake completion. Gates and the existing native
thread-exit observer cover admitted callback versus teardown and owner destruction;
watchdogs bound failures. No sleep is used as sole concurrency evidence.
An uninstalled seam build pauses immediately before stack admission and after
reservation, proving both detach orderings with successful and throwing listeners.
The installed provider contains neither hooks nor control switches. The second
fixture record has deliberately different bytes, so a held-buffer overwrite
cannot hide behind identical input payloads. Health tests verify running,
terminal-failed and retired states; changed immutable declarations are refused
before reopening the source.
Focused tests precede the final-source nine-lane matrix and installed/package
checks. Physical cameras are excluded. Execution results are recorded separately
in the [validation record](GIGE-MEL-044-validation.md).