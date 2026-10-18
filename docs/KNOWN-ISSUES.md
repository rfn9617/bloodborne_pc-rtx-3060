# Known issues — frozen 2026-10-18 release

| Symptom | Evidence / status |
|---|---|
| Brief black geometry, displacement or detail/aliasing changes during camera motion | Reported with FG OFF. Disabling OC did not remove it. Cause unresolved. A later CPU metadata journal was prepared, not gameplay-validated or promoted. |
| Persistent blur after travel, death/reload, sometimes more frequent in long sessions | One captured bad state already had blur before DLSS. This localizes that instance upstream, not the exact pass or every possible blur cause. Waiting10–15 seconds did not clear the reported state; another load could. |
| White blood edges/patches, possible decal projection through walls | Observed across reconstruction/FG settings and restarts; patches seen before DLSS. Exact shader/pixel attribution unresolved. |
| Small residual Mensis glow flicker | Guard v2 materially improved the user's result and was accepted; small occasional flicker remains. Unsupported glow permutations retain original code. |
| Memory stays allocated after travel/menu | GPU-safe retirement/GC improved observed behavior. Fixed pools, caches and allocator blocks can stay reserved. Complete leak elimination is not proven. |
| FG cape/doll fabric pulses or feels less smooth | User reported disappearance with FG OFF. Work paused; preserved preview is optional, no Reflex. Presented FPS does not establish display cadence. |
| First-use stutters | Shader/pipeline work can still occur. Motion cache compatibility checks intentionally reject unsuitable variants. Cold and warm caches are not comparable. |

Final default: FG, CACAO, captures and extended diagnostics OFF. No skip-uncompiled
draw workaround. A clean Vulkan/DLSS log does not prove correct pixels or vectors.
Use the [experiment record](EXPERIMENT-TEMPLATE.md); visually similar artifacts
need not share the same cause.
