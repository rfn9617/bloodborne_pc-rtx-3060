# Performance evidence

Machine: i7-11800H, 32 GB RAM, RTX 3060 Laptop 6 GB, 1920×1080, 144 Hz display.
Laptop power, temperature and game state affect repeatability. Play is capped at
72 FPS; comparisons below use separate uncapped runs. Do not benchmark captures
or heavyweight profiling as though they were ordinary gameplay.

| Comparison | Recorded result | Interpretation |
|---|---|---|
| Final OC, same heavy view confirmed by user | Baseline76.3/76.0/77.1; test83.1/83.1/83.2/82.7/83.7/82.3 FPS. Mean76.47→83.02, +8.57% | Local result. User confirmed geometry defects also existed in baseline. Not a full-route or all-build ranking. |
| Earlier CPU camera-constant fast path | Median59.6→74.1 FPS;16.69→13.29 ms | Separate runs, similar but nonidentical workload. User accepted image/hair; not an isolated universal24% claim. |
| Depth tile fusion | About+1.3% in comparable intervals | Small accepted local gain. Do not add this to percentages from other scenes. |
| Fresh upstream-base transplant | User estimated7–8% worse, frequent stutters | Rejected locally. Unequal cache warmth/compilation load prevents a controlled steady-state ranking. |
| Earlier FG overhead report | About89 real FPS without FG versus67 real /135 presented with FG | Substantial overhead at that stage; not a final FG benchmark or proof of uniform display pacing. |

OC evidence: `reports/accepted-occlusion-ab.json` and its source/test manifest.
These are correlated windows from a small gameplay test, not independent lab trials.
Record resolution, mode, exact view, enemies, warm-up, cap and instrumentation.

## Important limits

- Capped GPU utilization falling80%→75% indicates changed headroom, not automatically
  a5% increase in achievable FPS.
- DLSS does not guarantee a20% gain. CPU work, unchanged native-size passes and
  reconstruction overhead can dominate. Measure the same scene and pass timings.
- VMA reserved blocks, live allocations and total driver usage are different.
  External DLSS allocations and fallback budget estimates must be labelled.
- A visually simple scene can still submit hidden geometry, effects, shadows and
  expensive synchronization. Facing a wall does not imply an empty workload.
- There is no controlled ranking against every community fork. A fastest-build
  claim or total cumulative improvement percentage would be unsupported.

The accepted executable is preserved byte-for-byte. Clean-source build/tests
validate the exported implementation, not extended gameplay on every driver/PC.
