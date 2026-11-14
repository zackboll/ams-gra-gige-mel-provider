# Independently built matched-C++ profile module

This separate CMake project builds `librate_limited_profile.so`, not a provider
or SDK library. It implements the actual `ICameraProfile` and independently owned
`ICameraProfileState` interfaces. Its policy caps capabilities/configuration at
20 Hz and rejects odd native frame IDs. It compiles the project's SDK-free
representation algorithms into its own hidden archive; it links neither the
provider nor Aravis/Pleora. This is an example/test policy, not a qualified camera
profile. No camera is accessed by the module factory.

From the matching provider checkout, for a GCC Debug host:

```sh
cmake -S examples/profile_module -B /tmp/my-profile -G Ninja \
  -DGIGE_MEL_SOURCE_ROOT="$PWD" -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/my-profile --parallel 2
nm -D --defined-only /tmp/my-profile/librate_limited_profile.so
ldd /tmp/my-profile/librate_limited_profile.so

# SDK-enabled host previously built independently. Select only, no camera I/O:
./build/035-both-dev/gige_mel_select \
  --profile-module /tmp/my-profile/librate_limited_profile.so \
  aravis EXACT_DEVICE_ID test_rate_limited_v1
./build/035-both-dev/gige_mel_select \
  --profile-module /tmp/my-profile/librate_limited_profile.so \
  pleora EXACT_MAC_ID test_rate_limited_v1
```

The Pleora command needs its normal external SDK runtime environment. Selection
does not establish device existence, camera compatibility, licensing or time
qualification. Add `--inspect` only to explicitly open/read/close; it does not
configure or stream. Synthetic deliberately cannot execute this profile.

Use the exact host compiler/version, libstdc++, interface closure and ABI flags.
Match `CMAKE_BUILD_TYPE`, `CMAKE_CXX_FLAGS` and its configuration-specific variant,
and `GIGE_MEL_SANITIZERS` (empty, `address,undefined`, or `thread`). Sanitizer
runtimes must already be appropriate for the host; loading an instrumented
module is not a way to instrument an uninstrumented executable. The generated
identity includes the compiler version, libstdc++ date and dual-ABI setting,
debug-STL setting, C++ level, configuration/flags hash, sanitizer setting and
interface-header closure SHA-256. A mismatch fails explicitly before the factory.
Do not copy/edit the generated identity to bypass a mismatch.

Required headers: `profiles/profile_module.hpp` and its generated
`profile_build_identity.hpp`, plus the SDK-free transitive headers in this
checkout. The source tree is the initial development header bundle; no installed
SDK package or permanent binary ABI is promised. `module.cpp` contains the exact
C-linkage exported discovery declaration, descriptor, guarded factory and matching
noexcept destructor. `rate_limited_policy.hpp` contains the actual implementation.
All other symbols are local via `exports.map`; GNU unique symbols are disabled
for GCC to permit actual unload rather than glibc NODELETE retention.

Factories use `module::guarded_create` to contain allocation, standard and unknown
exceptions inside the C callback. Destroy must never throw. The host destroys
profiles using the descriptor's callback; state shared_ptr control blocks remain
module-defined but are released while the module is still pinned. Entry returns
a static borrowed descriptor valid until unload. There are no module worker
threads or asynchronous callbacks.

`GIGE_MEL_MODULE_NEGATIVE_TESTS=ON` builds deliberately invalid modules for the
host tests; never distribute these. The example's `GIGE_MEL_TEST_UNLOAD_LOG`
environment variable is test instrumentation: its ELF finalizer appends one line
to the named file. It neither pins nor manages modules. Leave it unset normally.
The host build uses an independent ExternalProject configure/build of this
project, not a static link of this policy into host executables.

Dynamic loading executes trusted code, including constructors before descriptor
validation. This is not a sandbox, authentication scheme, stable cross-toolchain
C ABI, hot replacement, backend plugin loader or search-directory mechanism.