# Source and release provenance

Classic base: `e372481574ec097506fb8f729636afb09ebfa27c`. It contains the earlier
`codex/rtx3060-windows-fixes` history. Local work after that base is committed in
this final branch. No force reset/reapplication of the old initial patch was used.

Accepted normal executable SHA-256:
`4d26fd1449ca9f887aeafd0feacb4a3c3d2bcb667b125cb0b0356332176014ed`.

Preserved older FG preview SHA-256:
`b76d822b36db8bd750ca2af8dabba3beb19a0cf4d98fa2f6f6a74cee8f7f7986`.
It is an earlier artifact, not a claim to compile from every later normal-path
change. Local historical source/patch records are retained separately.

The final ordinary source was reconstructed from the working classic tree and
the saved pre-CACAO/pre-journal backups. All23 files in the accepted OC/Ultra
Quality source manifest match the original build's saved hashes. The original
accepted OC implementation/test were recovered from the saved complete patch,
excluding a later unpromoted hold-count adjustment. Source files and manifest
are in `docs/reports/accepted-source-manifest.json`.

Packaging/launcher changes add a portable final runner, current profile defaults,
Ultra Quality selection and project documentation. They do not replace the
accepted renderer with the newest diagnostic executable. A clean-source renderer
build is tested separately; reproducibility here means source/build validation,
not a promise of bit-identical debug information across directories/toolchains.

Pinned submodules are recorded in `.gitmodules` and the Git tree. FSR-Vulkan is
`c64f093404125e960813f95f1996c14780f0dd69`; consolidated local overrides, including
optional FG work, are in `gpu/patches/fsr-vulkan/0001-final-local-overrides.patch`.
`build.sh` applies this once and recognizes an already applied patch. The large
generated/model payloads are not secretly pulled from another project directory.

Build: MSYS2 CLANG64, Clang22.1.8, C++23, RelWithDebInfo, LTO OFF, PGO OFF.
Dependencies and DLSS bridge build: `packaging/windows/README.md`.
The launcher is frozen with Python3.12 / PyInstaller; the release needs no Python.
NVIDIA bridge/runtime and their notices are included from the existing licensed
Windows package. Personal game/mod/save/cache data are excluded.

Original project and community work remains attributed in Git history,
`docs/UPSTREAM-README.md`, LICENSE, dependency notices and local audit records.
The OC work was selectively adapted from the reviewed upstream query approach;
it does not include the rejected memory policy or skip-uncompiled behavior.
