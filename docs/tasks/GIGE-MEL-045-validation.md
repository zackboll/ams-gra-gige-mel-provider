# GIGE-MEL-045 validation

SDK-free recording-qualified virtual stare only; no physical camera, gimbal,
pointing transform, UTC, tracking, scan performance or broad MEL qualification.
Default synthetic and production adapters remain unsupported.

Focused tests execute the real shared provider and a separate published-MEL-only
consumer translation unit. An uninstalled seam build gates ack reservation before
entry to prove detach drainage. Recording controls own/advance virtual task and
clock state. Gates/barriers establish ordering; deadlines are watchdogs.

Covered: Standby → TaskSched → accepted → executing → completed → Standby;
current abort, inaccessible AbortTask ID non-targeting, replacement, rejected
admission preserving old work, independent IDs including 0/UINT32_MAX, duplicates,
bounded exhaustion/reset, invalid coordinates/time/duration/priority/action,
unqualified/default source, failed open/cancel/observe and recovery/forced
retirement, callback exceptions/cross-manager reentry, concurrent duplicates,
Standby during execution, Control pin, no late metadata, reserved/entered callback
detach, Image/Stack exclusion/retained bytes and software Health state.

## Qualification status

Focused GCC task tests passed during development. Final-source nine-lane matrix,
installed consumers, isolation and integrity results are pending execution.
No readiness or hosted CI completion is claimed yet.