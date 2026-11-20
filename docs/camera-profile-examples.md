# Camera Profile Examples

These are **internal development references**, not installed public API or
SDK headers. Read [Create a Camera Profile](camera-profile-tutorial.md) for the
implementation sequence and the [C++ Interface](index.md#camera-profile-c-interface)
for the types. In the generated reference, each C++ file page links to its
line-numbered source. The CMake links go to the exact source revision on GitHub.

| Reference implementation | Profile source | Module entry point and descriptor | Standalone build |
| --- | --- | --- | --- |
| Cognex CIC-5000R (real-camera policy, fixture-qualified) | [Cognex profile implementation](../camera_profiles/cognex_cic_5000r/profile.cpp), [Cognex profile declaration](../camera_profiles/cognex_cic_5000r/profile.hpp) | [Cognex module entry point](../camera_profiles/cognex_cic_5000r/module.cpp) | [Cognex CMakeLists.txt](../camera_profiles/cognex_cic_5000r/CMakeLists.txt) |
| Rate-limited (teaching and contract-test policy) | [Rate-limited profile implementation](../examples/profile_module/rate_limited_policy.hpp) | [Rate-limited module descriptor](../examples/profile_module/module.cpp) | [Rate-limited CMakeLists.txt](../examples/profile_module/CMakeLists.txt) |
| Navigation record (optional navigation handler, fictional test record) | [Navigation record profile implementation](../examples/profile_module/navigation_record_policy.hpp) | [Navigation record module descriptor](../examples/profile_module/navigation_module.cpp) | Same project as rate-limited |

Start with the [profile and state interfaces](../src/profiles/camera_profile.hpp),
[typed feature service](../src/device_features.hpp),
[frame facts and decisions](../src/profiles/frame_policy.hpp), and
[matched-C++ module descriptor](../src/profiles/profile_module.hpp).
See [profile loader](../src/selection/profile_loader.hpp) for the host side.