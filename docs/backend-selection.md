# Backend Selection

Use [Current Capabilities and Limitations](status.md#backend-and-camera-profile-matrix)
to choose among the SDK-free synthetic source and optional adapters. An adapter's
availability does not imply support for every camera model, frame format, or
optional MEL capability.

- <!-- subpage --> [Runtime selection](runtime-selection.md) — select the compiled backend, exact device, and compatible profile; load a module explicitly when needed.
- <!-- subpage --> [Backend authoring](backend-authoring.md) — add a new SDK or transport only when an existing adapter cannot serve the device.

For a different model on the same backend, follow [Create a Camera Profile](camera-profile-tutorial.md).