# Lessons for the next PC runtime / port optimization project

This is a reusable working method distilled from the RTX3060 Bloodborne work.
Specific shader hashes, game patches and thresholds are not portable algorithms.
Begin a new project with the [experiment template](EXPERIMENT-TEMPLATE.md), not
by replaying every old patch.

## 1. Freeze a playable reference

Record source commit plus dirty changes, submodule commits/patches, compiler,
runtime/DLL versions, renderer hash and configuration. Keep the accepted binary
separate from the build output: the last successful compilation may be a rejected
experiment. Cache schema, driver warmth and graphics settings are part of the test.

Use one canonical save location when the tester makes real progress. Diagnostic
users should link to it, with separate configs/logs/caches as required. Never
restore an old snapshot merely to prepare a comparison. Close the game for an
intentional migration, back up replaced data and verify it. Do not publish private saves.

## 2. Establish the limiting work

Measure uncapped without captures, then profile separately. Compare an identical
view first and a route second; keep enemies, resolution, effects, cap and warm-up
known. Record median/frame-time distribution and long spikes, not just a single
FPS number. Low GPU utilization can mean CPU waits, bandwidth, synchronization
or scheduling limits. A lower internal resolution cannot accelerate unchanged
native-resolution work or CPU preparation.

Inspect draw preparation, command recording, copy waits, pipeline compilations,
GPU passes and present pacing as distinct quantities. A GPU timestamp, a driver
utilization percentage and a displayed FPS counter measure different things.
Do not infer a5% speedup from a5-point utilization reduction under a frame cap.

## 3. Check ownership and time before tuning quality

The correct camera is the one used by the scene draw, not necessarily the first
camera block seen that frame. Auxiliary views and reused guest constants require
pass identity, captured data and explicit previous/current-frame rules. A later
read from mutable guest memory can silently produce a different camera.

For asynchronous work, distinguish guest virtual address, allocation identity,
physical backing, resource generation and GPU completion. Same address does not
mean same object. Protection changes need not mean allocation replacement. Avoid
cross-thread reads of producer-owned state; publish an appropriate snapshot.

Generated walkers should recover per invocation after null/faults. Permanently
rewriting a load into zero can poison future valid data. An absent optional vertex
attribute should not automatically invalidate the whole mesh. Stable storage
addresses matter when caches/threads retain references to container elements.

## 4. Localize a visual defect across stages

Distinguish guest input, motion/depth, reconstruction output, postprocess, UI and
final presentation. A defect already present before DLSS is not established as
a DLSS defect. Similar-looking black objects with and without FG need not share
a cause. A normal Vulkan log does not prove correct geometry or motion.

Prefer bounded low-overhead metadata first. If pixels are essential, use a bounded
capture only with the user's cooperation and measure outside the capture. A
rolling metadata log can preserve context after a brief event, but cannot prove
what transformed GPU vertices or pixels contained. State that limit explicitly.

Hair shimmer in this project survived larger history capacity and repaired palette
selection; disabling extra alpha mip sharpening was the effective user-observed
change. Measure capacity skips before enlarging pools. More sharpening is not
always more stable detail. Do not lower vector guards blindly: legitimate motion
can be large too.

## 5. Treat memory as several measurements

Separate live allocations, reserved allocator blocks, fixed pools, caches, staging
buffers, external reconstruction allocations and driver budget/usage. Label
fallback estimates. Repeat location transitions and inspect plateaus/retirement;
nonzero menu VRAM by itself does not establish a leak.

Destroy or reuse resources only after the relevant GPU completion point. If dirty
GPU data must survive eviction, verify writeback/recreation or keep the resource.
Bound cleanup work per time interval; a large synchronous collection can turn a
memory improvement into frame-time spikes.

## 6. Import upstream changes selectively

Record the source revision and what it actually changes. A newer branch may have
different synchronization, cache formats or memory architecture. Matching function
names do not establish compatibility. Test null/remap/ABA cases and failure paths,
not only normal draws. Preserve original attribution and licenses.

For occlusion, absent/late/invalid query ownership must fail visible. Validate
writeback against current backing and foreign writes. Shader compilation latency
must not be disguised by silently dropping geometry.

For a new AO pass, replace the intended lighting contribution rather than darken
the final image twice. Technical integration and synthetic GPU tests are necessary
but insufficient: CACAO's correct mask replacement was still visually rejected.

## 7. Keep failed experiments as results

Zero gain, image regression and failed hypotheses are useful data. Material CSE,
UBO, clip fusion, alternate copy waits, motion capacity expansion and a fresh
upstream transplant should not be repeated without new evidence. Record why an
experiment was accepted, rejected or left unknown and which flags remain OFF.

FG is a separate workload/pacing problem. Measure real FPS, generated FPS,
latency, memory and presentation cadence independently. Validate color formats,
UI composition, motion/depth conventions, disocclusion and SDK lifetimes. Do not
promise72 real+72 generated without measuring overhead. Reflex was not implemented
here; a frame cap or V-Sync is not its replacement.

## 8. Ship evidence and clean safely

Freeze the tested executable, publish matching source and dependency patches,
identify exact hashes, describe known issues, and keep benchmark limits visible.
Prefer a small credible measured claim to an unsupported fastest-build ranking.
Archive decisions and reusable scripts; omit game dumps, caches and personal data.

Before deleting tests, inventory junctions and dependencies. Unlink reparse points
without traversing them, verify absolute cleanup roots and protect retained tools.
Confirm the remote release and its checksums before deleting the local playable
copy. Save deletion requires the user's explicit choice; this project obtained it
after completion. Retain only the documentation/SDKs the user requested.
