# RTX 3060 6 GB edition — frozen playable release

A Windows build focused on1080p gameplay on the i7-11800H /32 GB /RTX3060 Laptop6 GB
test system. This packages the user's accepted classic renderer after an extended
optimization/debugging session, with its source, build notes and honest limitations.

## Highlights

- Camera/constant handling improvements for the sewer reconstruction path while
  retaining object motion vectors.
- Conservative occlusion culling: about76.5→83.0 FPS uncapped in the confirmed
  matched heavy view (+8.6% locally; not a whole-game or all-fork comparison).
- DLSS K Quality plus custom75% Ultra Quality, with existing preset IDs retained.
- Accepted depth/tile fusion, CPU/recording paths, resource-safety work and Mensis
  bloom guard. Original ambient occlusion retained.
- Default1080p /DLSS K Ultra Quality /72 FPS /Mailbox /one frame ahead /FG OFF.
- Source provenance, measured-results table, rejected experiments and reusable
  engineering handbook/template are included in the repository.

Extract, open **Bloodborne.exe** to select your own decrypted1.09 game directory,
then use **Play RTX3060.cmd**. The package contains no game files, personal mods,
saves, account settings or shader cache. SHA256SUMS identifies the downloadable
archive; release-manifest.json identifies the renderer and optional FG binary.

## Read before use

Rare geometry flashes/detail instability, occasional persistent reload blur,
blood/decal artifacts and small residual glow flicker are still open. Memory
cleanup improved in observed play, but complete leak elimination is not proven.
See the repository's known-issues page. This is not a fastest-build claim.

An older accepted FG preview remains optional; clothing pulses/overhead persist,
Reflex is not implemented, and it lacks later ordinary-renderer changes. It is
not the recommended default. CACAO and the rejected fresh upstream-base transplant
are not part of the normal renderer. No new contact shadows/SSGI/RT are claimed.

Спасибо за интерес к проекту. Русское описание, замеры и известные ошибки — в
docs/README-RU.md. Полезные отчёты о других GPU приветствуются с точными настройками,
одинаковым ракурсом и раздельными измерениями обычного FPS и профилировщика.
