# MEL Capabilities

Use [Current Capabilities and Limitations](status.md#mel-behavior-summary) to
distinguish supported behavior from partial, fixture-only, and unavailable
paths. A documented interface or channel stub is not a conformance claim.

- <!-- subpage --> [Implementation Profile](implementation-profile.md) — provider behavior, memory ownership, and explicit limits.
- <!-- subpage --> [Image and Frame Metadata](image-frame-metadata.md) — Mono8/Mono12, frame layout, and source metadata.
- <!-- subpage --> [Camera Configuration and C2](configuration.md) — camera commands, ConfigSet, ModeCmd, and unsupported-command semantics.
- <!-- subpage --> [Health and BIT](health-status.md) — partial Health publication, software BIT, and subsystem status.
- <!-- subpage --> [Navigation and LOS](navigation-los.md) — partial navigation receipt, optional forwarding, and source-backed LOS.
- <!-- subpage --> [Stacked Images](stacked-image-provider.md) — fixture-qualified, source-dependent foundation; not advertised by the default synthetic or production adapters.
- <!-- subpage --> [Virtual Stare Tasking](task-scheduling-provider.md) — recording-qualified, Immediate-only admission and current abort; no physical pointing or MEL completion reports.

Tracking and general/physical scheduling remain unsupported. See [MEL Requirements Traceability](mel-traceability.md)
for required, conditional, and optional operations and their evidence limits.