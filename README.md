# Bloodborne PC — RTX 3060 6 GB edition

**A Windows performance fork focused on consistent 1080p gameplay on a 6 GB laptop GPU.**
Tested through extended gameplay on an i7-11800H, 32 GB RAM and RTX 3060 Laptop 6 GB.
The final profile targets 72 real FPS, with object motion vectors and DLSS K;
Quality and a custom 75% Ultra Quality mode are available. This freezes the
user-tested classic renderer with selected fixes rather than a wholesale upstream rebase.

[Download Windows release](https://github.com/rfn9617/bloodborne_pc-rtx-3060/releases/tag/rtx3060-final-2026-10-18)
· [Русский](docs/README-RU.md)
· [Measurements](docs/PERFORMANCE.md)
· [Known issues](docs/KNOWN-ISSUES.md)
· [Engineering handbook](docs/ENGINEERING-HANDBOOK.md)

## What makes this build different

- **Camera and motion correctness first.** Largest-scene camera selection and
  same-draw constant snapshots address the sewer reconstruction path. The accepted
  fast path removes repeated CPU camera scans while retaining object vectors.
- **Conservative occlusion culling.** Real visibility queries, mapping/backing
  ownership checks, asynchronous writeback validation and fail-visible handling.
  In the user's matched heavy view, windows went from about 76.5 to 83.0 FPS
  uncapped: approximately **+8.6%**. This is a local result, not a game-wide promise.
- **Less avoidable CPU/GPU work.** Retains the inherited threaded recording and
  palette-hash fast paths, direct DLSS output/sharpening path, depth tile fusion,
  safe scheduler handover and GPU-safe resource retirement. The change inventory
  distinguishes inherited features from this fork's later work.
- **DLSS Ultra Quality at 75% per axis.** 1440×810 nominal input at 1920×1080;
  the game can align the scene to 1440×812. This uses explicit custom dimensions,
  not an official NVIDIA NGX UltraQuality enum.
- **Practical image settings.** Original AO, object vectors ON, extra alpha mip
  sharpening OFF, accepted Mensis bloom guard, motion blur/jitter OFF. Quality
  remains selectable for a lower rendering cost.
- **Evidence retained.** Accepted/rejected experiments, known defects and reusable
  port-development notes are documented. Another player's cache or saves are not required.

## Install and play

1. Extract the release to a writable directory. Keep an existing installation
   separate when first trying it.
2. Open `Bloodborne.exe`, select your own decrypted Bloodborne 1.09 game directory
   and save the launcher settings.
3. Use **`Play RTX3060.cmd`** for the frozen final profile. The normal launcher
   also uses the final renderer. **Insert** opens in-game settings.

Default: 1080p / DLSS K Ultra Quality / 72 FPS / Mailbox / one frame ahead / FG OFF.
Choose Quality if a scene is GPU-limited. Mailbox does not guarantee a new frame
on every refresh; 72 real FPS on a 144 Hz display repeats frames. Neither 144 real
FPS nor guaranteed 72 FPS in every scene is promised.

**Game files, saves, personal mods, account settings, caches and gameplay captures
are not included.** You supply the game. Back up your own saves/settings before
switching installations. Local finalization preserved the tester's progress in place.

An older accepted **FG preview** remains optional. It has the display-format and
object-motion guards, but clothing pulses, overhead and occasional artifacts are
unresolved. It is OFF by default, has no Reflex, and lacks later ordinary-renderer
changes. Use `Play FG preview.cmd` only to try that preserved experiment.

## Status

This is **not a fastest-build claim**: there is no controlled comparison of every
community fork on matching settings. Rare brief black/geometry flashes, persistent
blur after some reloads, blood/decal artifacts and small residual glow flicker
remain open. VRAM cleanup improved in observed gameplay, but complete leak
elimination or empty menu VRAM is not established. Read [known issues](docs/KNOWN-ISSUES.md).

CACAO was rejected for flatter environmental lighting and is absent from the
final renderer. A fresh upstream-base transplant was also rejected locally.
Material CSE/UBO/clip fusion and several copy/binding experiments did not establish
a useful gain and remain disabled. These decisions are part of the project record.

## Source and contributions

Classic base: `e372481574ec097506fb8f729636afb09ebfa27c`, plus this branch's changes.
The release identifies the accepted executable by SHA-256. A clean rebuild is
tested separately and not silently substituted for the user's tested binary.
See [provenance](docs/SOURCE-PROVENANCE.md), [changes](docs/CHANGELOG-RTX3060.md)
and [Windows build instructions](packaging/windows/README.md).

Start contributions with the [experiment template](docs/EXPERIMENT-TEMPLATE.md):
matched view, frame times, configuration and a visual check. A clean Vulkan log
does not prove a correct image; percentages from different scenes do not add up.

## Credits and license

Based on the Bloodborne PC/runtime and Windows-port community, including the
shadPS4-derived renderer, Windows integration, FSR-Vulkan and their contributors.
Original documentation/attribution: [UPSTREAM-README.md](docs/UPSTREAM-README.md).
See [LICENSE](LICENSE); third-party notices accompany the binary. Local profiling,
adaptation, testing and the RTX3060 release were maintained in this fork.
NVIDIA DLSS is an optional separately licensed runtime.
