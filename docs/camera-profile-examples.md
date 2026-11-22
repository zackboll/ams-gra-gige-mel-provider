# Camera Profile Examples

These are **internal development references**, not installed public API or
SDK headers. Read [Create a Camera Profile](camera-profile-tutorial.md) for the
implementation sequence and the [C++ Interface](index.md#camera-profile-c-interface)
for the types. In the generated reference, each C++ file page links to its
line-numbered source. The CMake links go to the exact source revision on GitHub.

| Reference implementation | Profile source | Module entry point and descriptor | Standalone build |
| --- | --- | --- | --- |
| Cognex CIC-5000R (real-camera policy, fixture-qualified; inherits [monochrome defaults](../src/profiles/mono_free_running_profile.hpp)) | [Cognex profile implementation](../camera_profiles/cognex_cic_5000r/profile.cpp), [Cognex profile declaration](../camera_profiles/cognex_cic_5000r/profile.hpp) | [Cognex module entry point](../camera_profiles/cognex_cic_5000r/module.cpp) | [Cognex CMakeLists.txt](../camera_profiles/cognex_cic_5000r/CMakeLists.txt) |
| Rate-limited (teaching and contract-test policy; uses composition) | [Rate-limited profile implementation](../examples/profile_module/rate_limited_policy.hpp) | [Rate-limited module descriptor](../examples/profile_module/module.cpp) | [Rate-limited CMakeLists.txt](../examples/profile_module/CMakeLists.txt) |
| Navigation record (optional navigation handler, fictional test record) | [Navigation record profile implementation](../examples/profile_module/navigation_record_policy.hpp) | [Navigation record module descriptor](../examples/profile_module/navigation_module.cpp) | Same project as rate-limited |
| Virtual optics (optional Image FOV; inherits monochrome defaults) | [Virtual optics profile and module](../examples/profile_module/virtual_optics_module.cpp) | Same file, unchanged v1 discovery | Same project as rate-limited |

Start with the [profile and state interfaces](../src/profiles/camera_profile.hpp),
[typed feature service](../src/device_features.hpp),
[frame facts and decisions](../src/profiles/frame_policy.hpp), and
[matched-C++ module descriptor](../src/profiles/profile_module.hpp).
See [profile loader](../src/selection/profile_loader.hpp) for the host side.

## Inherited virtual optics

`test_virtual_optics_v1` explicitly constructs a `MonoFreeRunningProfileState`
subclass from `open()`, reusing configuration, readback and preflight. The
unchanged factory passes explicit **virtual** deployment constants: sensor
384×256, focal lengths `(140, 95)` pixels and principal point `(192, 128)` in
pixel-edge coordinates. Edges start at `(0,0)` at the top left, increasing right
and down. A nonempty ROI must fit completely inside those sensor bounds. Each
angular extent is the difference of `atan2(edge - principal_point, focal_length)`
at the upper and lower ROI edges on that axis. An 80×60 ROI near the corner
and the same size ROI centered on the principal point produce different values
on both axes. This model is not an assumption about other profiles or lenses.

Build [the standalone project](../examples/profile_module/README.md) with the
host's matched C++ and build settings. It produces `libvirtual_optics_profile.so`.
With the module contract fixtures enabled, it also builds a variant combining
FOV and the fictional navigation record. The SDK-free FOV tests use the
ordinary module loader and public MEL Image delivery. The module has no SDK
or provider dependency. No production adapter forwards this extension.