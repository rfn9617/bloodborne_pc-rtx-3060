# Work inventory and experiment decisions

Cumulative fork of classic `e372481`, tested on the RTX3060 laptop during
8–18 October2026. Git history preserves inherited Windows/runtime/shadPS4 work;
this document does not claim every mechanism was invented in this fork.

## Ordinary accepted renderer

**Camera/reconstruction:** largest G-buffer camera, same-draw snapshots instead
of later reads from reused guest constants, lazy/fast CPU selection, reprojection
guards and auxiliary-view handling. Addressed the severe sewer path; remaining
intermittent artifacts are not declared solved.

**CPU submission:** retains inherited threaded preparation/recording, stage-A
palette hashing, faster residency/index/sampler paths and safe recorder handover.
Later work preserved these when rejecting a fresh-base transplant. No new blanket
async-compute or PGO gain claimed; this Windows build uses LTO OFF / PGO OFF.

**GPU passes:** direct DLSS/sharpening output, depth/tile fusion and selected
read-after-read synchronization reductions. Native-size effects can limit the
benefit from lowering scene resolution.

**Resource safety:** GPU-safe retirement, bounded cleanup, motion-history expiry,
texture upload/readback coherence and guarded writeback. The new upstream VRAM
allocation policy was excluded. Measure live/reserved/driver memory separately.

**Generated shader/data robustness:** stable slot storage; recoverable SRT
null/fault handling rather than permanently disabling future valid loads; valid
optional null vertex bindings instead of dropping an entire draw; motion/plain
pipeline permutation checks and session-address cache handling. Earlier texture
experiments that regressed color/geometry were isolated, not blindly promoted.

**Image/glow:** alpha_detail0 was accepted after motion-capacity/palette changes
alone did not cure hair noise. Object vectors remain ON. Mensis bloom guard v2
reduces observed flicker; original AO and unsupported original glow permutations
remain. Motion blur/jitter are OFF in the accepted play profile.

**Final OC/Ultra Quality:** conservative visibility queries, missing-query
fail-visible behavior, timeline-aware resolve, guest mapping/backing ownership
and guarded asynchronous writeback. Preset5 adds75% per-axis input without
renumbering0–4 or changing the DLSS bridge. The original accepted OC query variant
is shipped; a later optional hold-count adjustment was not substituted.

## Optional FG research

FSR FG, display sampling/color-format correction, UI composition, SDK lifetime,
descriptor/constants reuse, optical-flow search optimization and object-relative
motion guards. Several black objects, wall speckles and color errors disappeared
in user tests. Clothing pulses/overhead remain, so FG is not the default. The
preserved FG executable predates later ordinary fixes and is labelled accordingly.

## Disabled / rejected / unconfirmed

| Experiment | Decision |
|---|---|
| Material UBO/CSE, clip fusion/early tests | No useful proven gameplay gain; OFF. |
| Range-aware host-copy waits, GPU vector copy, binding cursor | Reports retained; no unproven promotion. Final uses all waits/reference paths. |
| Expanded/compact motion pool | Recorded route did not exhaust ordinary capacity; compact OFF, ordinary8 Mi-vertex pool. |
| Large-palette classification as hair cure | Selection repaired, insufficient visual improvement; alpha sampling was effective in that test. |
| Fresh upstream-base transplant | Rejected for stutter/worse observed performance; classic retained. |
| CACAO | Mask replacement worked technically, environmental lighting looked flatter/weaker; rejected and removed from final renderer. |
| Geometry metadata journal | Later unvalidated diagnostic; archived, not shipped as accepted renderer. |
| Reflex, new contact shadows, SSGI/RT-style lighting, DX11/DX12 backend | Not implemented. |

Dated reports/scripts remain in the local final work archive. Public summaries
exclude personal saves, game dumps, caches and large captures. Rejected work is
preserved as knowledge, not advertised as enabled optimization.
