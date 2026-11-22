# Image and Frame Metadata

- [Image layout and synthetic camera](implementation-profile.md#image-layout-and-synthetic-camera) — Mono8 and the SDK-free source, with the adjacent [time conventions](implementation-profile.md#time-conventions) and [buffer ownership](implementation-profile.md#registered-host-memory-and-release).
- [Frozen Image field of view](implementation-profile.md#frozen-image-field-of-view) — default linear pitch or an explicit source for the run's actual ROI.
- <!-- subpage --> [Unpacked Mono12](mono12-image-profile.md) — admitted representation, row layout, and rejection rules.
- [Navigation and LOS](navigation-los.md) — optional source metadata and the limits on association with images.
- [Installed frame contract](https://zackboll.github.io/ams-gra-gige-mel-provider/api_installed.html#installed_frame_ownership) — descriptors, timestamps, and lease ownership in C++.