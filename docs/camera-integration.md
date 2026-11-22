# Camera Integration

For a new model on an existing SDK, start with a camera profile. A new SDK or
transport needs a backend, not a camera-specific branch in the MEL provider.

1. Choose an existing adapter with <!-- subpage --> [Backend Selection](backend-selection.md).
2. Select the exact device and model with [runtime selection](runtime-selection.md).
3. <!-- subpage --> [Create a Camera Profile](camera-profile-tutorial.md) — implement or load the profile for that device, then compare the [Cognex and teaching examples](camera-profile-examples.md).
4. Use the [authoring contract](camera-profile-authoring.md) to check feature admission, configuration readback, and frame validation.
5. Check which [MEL capabilities](mel-capabilities.md) require extra sensor support, such as LOS or stacked imagery.
6. Use the tutorial's [qualification checklist](camera-profile-tutorial.md#8-qualify-your-profile) to validate with fixtures before authorized hardware qualification.

## Backend guides

- <!-- subpage --> [Aravis Integration](genicam-sdk-guide.md) — native acquisition, timing gates, and fixture records.
- <!-- subpage --> [Pleora Integration](pleora-guide.md) — native reception, licensing, and evaluation limits.
- <!-- subpage --> [GenTL Status and Limitations](gentl-status.md) — advanced, internal work; not a streaming option.