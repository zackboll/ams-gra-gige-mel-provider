# Dynamic profile modules: pre-implementation design

Base commit `1523b8b278fb864c539298943bd2100875caa151`, tree
`d72f68b9614bccdac268e2cc5d230b75ef4ea1e9`. PR #28 verified merged before
creating `feature/gige-mel-035-dynamic-profile-loading`.

## Boundary and compatibility

Linux x86-64 only; opt-in absolute path, `RTLD_NOW | RTLD_LOCAL`, exact symbol
`gige_mel_camera_profile_module_v1`. A C-linkage discovery function returns a
borrowed descriptor with fixed-width version/size fields, bounded inline strings,
explicit Aravis/Pleora compatibility bits, and noexcept create/destroy callbacks.
The factory reports errors into a host-owned bounded buffer and returns a raw
profile whose matching destruction operation belongs to the module. Discovery,
creation and destruction must catch exceptions inside the module, not let them
cross the C callback boundary. An unexpected factory exception is translated by
the module's factory guard. Nulls, malformed metadata, wrong IDs and unsupported
versions/builds fail without fallback. No device open occurs during selection.

This is NOT a stable cross-toolchain C ABI. After validation it passes C++ virtual
objects, standard-library values and shared state ownership. Compatibility is an
explicit matched compiler/stdlib/ABI/interface-header revision contract, including
sanitizer mode. A header-closure hash detects interface edits; compiler and
standard-library configuration are checked as well. Metadata is not authentication
and cannot prove that a module was built honestly. Modules are trusted executable
code; constructors run at dlopen. No discovery/search/download is introduced.

## Ownership audit and proposed unload ordering

Existing Aravis device cores own profile + policy; stream wrappers retain the
core, while native-buffer leases retain their independent storage run (no plugin
code needed after retirement). Pleora device state owns profile + policy;
the copy-stream validator captures policy; retained copy leases own host bytes
without plugin code. States deliberately can outlive their parent profile.

The loader will wrap BOTH profile and each returned state in host-compiled
forwarders, each independently holding a shared RAII module owner. Profile
destruction invokes the module destroy callback before releasing its module
owner. State destruction releases its plugin-owned shared_ptr (including its
control block/deleter) before releasing its module owner. Thus no dependency on
the parent profile's lifetime is added. Catalog factories retain the loaded
profile wrapper; devices/streams/leases retain the wrappers through the existing
ownership chains. Synchronous calls pin their owners under existing native
contracts; no new asynchronous callbacks or operation locks are added.

The last module owner calls dlclose, never an explicit force-unload API and never
an intentional leak. Tests observe actual module finalization, not only wrapper
destruction. Failure cleanup follows the same ordering. Stop, close and immutable
retained bytes remain native-backend contracts, not a new common stream model.

## Composition and evidence plan

Load and validate before final built-in catalog construction. Register exact
validated IDs, and add them only to backends explicitly declared by descriptor
metadata AND accepted by composition. Synthetic remains default-only. No SDK
types enter the descriptor or profile headers; no module/backend dependency is
linked into the installed provider. Existing zero-argument factories stay intact.

A separate CMake project builds a real policy module with <=20 Hz admission and
odd-frame rejection, compiling shared representation algorithms locally without
linking provider or SDKs. Existing native profile scenarios will additionally
execute this module through Aravis Fake and Pleora GenApi/SDK-buffer fixtures.
Negative module variants and SDK-free state-lifetime/unload tests cover failures,
independent sessions and repeated teardown. All existing sanitizer/native suites,
installed isolation, pinned-byte and publication checks remain required. Offline
Pleora fixtures are not physical-device or license qualification.