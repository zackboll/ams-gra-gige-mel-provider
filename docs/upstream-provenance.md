# Upstream interface provenance

The provider implements the native C++ AMS-GRA IR MEL boundary. It does not
define a replacement MEL API and does not depend on the consumer-side
`ams-gra-mel-bridge` at runtime.

## Immutable source versions

| Component | Repository | Commit |
| --- | --- | --- |
| IR MEL | [open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/tree/8d9224519f12b44e0b28815755c56a32a28d24a0) | `8d9224519f12b44e0b28815755c56a32a28d24a0` |
| Common MEL | [open-arsenal/ams-gra-hello-world-sk-interfaces-common-mel](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-common-mel/tree/f6908437d8fd2f7fb69896f9eb9cfd272d10c439) | `f6908437d8fd2f7fb69896f9eb9cfd272d10c439` |
| AMS Math | [open-arsenal/ams-gra-hello-world-sk-libraries-ams-math](https://github.com/open-arsenal/ams-gra-hello-world-sk-libraries-ams-math/tree/00be45190f0e47d268cece8b8c2f8fb58b5418d2) | `00be45190f0e47d268cece8b8c2f8fb58b5418d2` |

Both MEL source families identify API version `4.0`. This string, the exact
source revisions above, the provider's numeric `VersionInfo`, and the local
project release are separate version concepts. Source compatibility does not
establish binary compatibility between unrelated C++ toolchains.

## Selected declaration closure

The root headers are:

- `irmel/library/irmel-types/Control.h`
- `irmel/library/irmel-types/Buffer.h`
- `irmel/library/irmel-types/ImageListener.h`
- `irmel/library/irmel-types/FrameHeader.h`
- `irmel/library/c2/C2Channel.h`
- `irmel/library/image/ImageChannel.h`
- `irmel/library/health-status/HealthStatusChannel.h`

The transitive published include graph contains **91 headers**: 61 IR MEL,
28 Common MEL, and two AMS Math headers (`Quaternion.h` and `RangeAzEl.h`).
Including the two retained notices per source family gives **97 locked files**.
Every file was compared to the Git blob identity returned by the corresponding
immutable upstream Git tree before the SHA-256 manifest was written. Files are
copied byte-for-byte, including whitespace and line endings. There are no
local patches to these declarations.

No upstream compiled implementation is required for this slice. The upstream
full AMS Math project has broader dependencies; they are not dependencies of
the two declarations actually reached here. In particular, the initial closure
does not contain TrackDataUpdate, RF MEL, AMS VITA, or Boost. Adding another
published interface can enlarge the closure and must be reviewed explicitly.

Header presence is not an implementation or conformance claim. Many optional
MEL types appear because they are referenced by the selected abstract classes.
See the provider's scope documentation for what the bootstrap actually does.

## Factory ABI

The upstream factory signatures are:

```cpp
extern "C" std::shared_ptr<API_Manager>
getAPI_Manager(const std::string& instance);

namespace ams::iface::irmel {
extern "C" std::shared_ptr<Control>
getControl(std::string_view instance, std::shared_ptr<::API_Manager> manager);

extern "C" std::shared_ptr<Buffer>
getBuffer(std::string_view instance, std::shared_ptr<::API_Manager> manager);
}
```

The `extern "C"` spelling stabilizes exported symbol names for dynamic lookup.
Arguments, return values, virtual interfaces, and ownership are still C++ ABI
objects. Provider and consumer need compatible architecture, compiler, standard
library ABI, RTTI, exception settings, and pinned declarations. A version query
cannot safely negotiate an incompatible ABI before the first factory call.

The provider must remain loaded while any factory result, channel, buffer,
future, callback target, vtable, or shared-pointer deleter can refer into it.
An explicit `Buffer::release()` returns the image slot for reuse; dropping a
`shared_ptr` alone is not the published reuse operation. Caller-supplied host
memory has its own lifetime and is not owned merely because a MEL buffer
object refers to it.

## Maintenance and verification

`third_party/manifest.json` is the dependency lock and exact file inventory.
The verification command performs no network access:

```sh
python3 scripts/vendor_upstream.py --verify
```

The command rejects missing, changed, or unexpected files inside each locked
component directory. It checks byte counts, SHA-256, and Git blob SHA-1. The
manifest is reviewed project input; checksum verification is not a substitute
for verifying the manifest's revisions and source provenance during upgrades.

`--restore` downloads only exact revision URLs from the existing manifest and
checks all files before replacing them. It does not choose releases, silently
fetch during CMake configuration, or alter the manifest. An offline restore
can use `--source-dir` as described in `third_party/README.md`.

## Licenses and notices

Each of the three source repositories supplies an Apache License 2.0
`LICENSE` and a federal-government `INTENT.md`. Both original files are
retained under its component directory and remain applicable to those files.
The upstream license SHA-256 is
`2f1aa718ddb1a34ed1581e3c57dddd1bb516fb7657a9c4076e8ff95b91d9844d`.
The root project license, if selected, governs the new implementation according
to its own terms; it does not replace the upstream notices.

The bootstrap was initially compiled with GCC 13 using these unmodified
declarations. Compiler/build/test results for the complete implementation are
recorded separately; no camera-hardware compatibility follows from this
declaration compilation.
