#!/usr/bin/env bash
# Builds dist/bbport-windows/ (and dist/bbport-windows.zip): Bloodborne.exe (the launcher, frozen
# with PyInstaller so players need no Python), bb-probe.exe with the MSYS2 CLANG64 DLLs it needs,
# the preparation scripts and run.py. Run from an MSYS2 CLANG64 shell after `bash build.sh`.
# Freezing uses a Windows Python 3.10+ (python.org; WINPYTHON overrides) and a private venv in
# out/pyenv with PyInstaller. FSR 4 assets in fsr4_shaders/ are included when present.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
profile=
minimal=0
while [[ $# -gt 0 ]]; do
    case $1 in
        --profile)
            [[ $# -ge 2 && $2 == rtx3060-6gb ]] || { echo 'Supported profile: rtx3060-6gb' >&2; exit 2; }
            profile=$2; shift 2 ;;
        --minimal) minimal=1; shift ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ $minimal == 0 || -n $profile ]] || { echo '--minimal requires --profile rtx3060-6gb' >&2; exit 2; }
package=bbport-windows
[[ -z $profile ]] || package+=-$profile
if [[ -n $profile ]]; then
    for required in bbport_dlss.dll nvngx_dlss.dll NVIDIA-DLSS-LICENSE.txt; do
        [[ -s out/$required ]] || { echo "Profile needs out/$required: run packaging/windows/build_dlss.sh first" >&2; exit 1; }
    done
fi
source ./msys2-env.sh
[[ -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe && -f out/bb-play.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

# A Windows Python (not MSYS2's) for PyInstaller.
python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
# Windows Python needs USERPROFILE (some MSYS2 shells start without it).
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller
# The scripts run inside Bloodborne.exe (--script): the standard modules they import come along.
hidden=()
for module in argparse base64 collections hashlib json re shutil struct tempfile xml.etree.ElementTree \
              urllib.request ctypes.wintypes; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name Bloodborne --icon "$(cygpath -w "$PWD/launcher/bloodborne.ico")" --distpath out/pyi-dist \
    --workpath out/pyi-work --specpath out/pyi-work --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    "$(cygpath -w "$PWD/launcher/bbport_launcher_win.py")"

# The package is assembled in a fresh staging folder and zipped from there; dist/bbport-windows
# (a playable copy that may hold saves and settings) is only refreshed afterwards.
dest=out/stage/$package
rm -rf -- "$dest"
mkdir -p "$dest/bin" "$dest/launcher"
cp -r out/pyi-dist/Bloodborne/. "$dest/"
llvm-strip -o "$dest/Play Bloodborne.exe" out/bb-play.exe
cp launcher/bloodborne.ico launcher/bloodborne.png "$dest/launcher/"
# The executables without debug information (out/ keeps the symbols for crash reports).
for exe in bb-probe.exe bb-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# Every DLL the executables load from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/bb-probe.exe" "$dest/bin/bb-gpu-capabilities.exe" |
    awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/bin/"
    done
cp -r scripts patches "$dest/"
cp run.py LICENSE README.md packaging/windows/README-Windows.txt "$dest/"
if [[ -n $profile ]]; then
    args=(--write "$dest/windows-profile.json")
    [[ $minimal == 0 ]] || args+=(--minimal)
    python3 scripts/windows_profile.py "${args[@]}"
    cp packaging/windows/RTX3060-6GB.md "$dest/"
    # --play and direct run.py must receive the same renderer defaults as the launcher.
    python3 - "$dest" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts')
from windows_profile import INI
Path(sys.argv[1], 'bbport.ini').write_text(
    '# RTX 3060 6 GB defaults; edit in the launcher or in-game menu\n' +
    ''.join(f'{key}={value}\n' for key, value in INI.items()), encoding='utf-8')
PY
fi
if [[ $minimal == 0 && -d fsr4_shaders ]]; then cp -r fsr4_shaders "$dest/"; fi
# DLSS (NVIDIA RTX): the MSVC-built bridge and NVIDIA's runtime, next to bb-probe.exe
# (packaging/windows/build_dlss.sh). Without them the DLSS option stays unavailable.
if [[ -f out/bbport_dlss.dll && -f out/nvngx_dlss.dll ]]; then
    cp out/bbport_dlss.dll out/nvngx_dlss.dll "$dest/bin/"
    mkdir -p "$dest/licenses" && cp out/NVIDIA-DLSS-LICENSE.txt "$dest/licenses/"
    cp gpu/dlss_bridge/LICENSE.txt "$dest/licenses/bbport_dlss-LICENSE.txt"
else
    echo "DLSS bridge not built (packaging/windows/build_dlss.sh): no DLSS in this package" >&2
fi
find "$dest" -name __pycache__ -prune -exec rm -r {} +
mkdir -p dist
rm -f "dist/$package.zip"
(cd out/stage && powershell -NoProfile -Command \
    "Compress-Archive -Path '$package' -DestinationPath '../../dist/$package.zip'")

# Refresh dist/bbport-windows, keeping what players create there (saves, settings, mods), and
# only while nothing runs from it: deleting a running launcher's files breaks it.
play=dist/$package
running=$(powershell -NoProfile -Command \
    "@(Get-Process | Where-Object { \$_.Path -like '$(cygpath -w "$PWD/$play")\\*' }).Count" | tr -d '\r')
if [[ ${running:-0} != 0 ]]; then
    echo "$play is in use ($running processes): not refreshed; the zip is ready." >&2
else
    mkdir -p "$play"
    find "$play" -mindepth 1 -maxdepth 1 ! -name user ! -name mods ! -name bbport.ini \
        ! -name mods.json ! -name patches.json -exec rm -rf {} +
    # Keep saved renderer settings; the profile can be reapplied explicitly in the launcher.
    saved_ini=0
    [[ ! -f $play/bbport.ini ]] || saved_ini=1
    for item in "$dest"/* "$dest"/.[!.]*; do
        [[ -e $item ]] || continue
        [[ $saved_ini == 0 || $(basename "$item") != bbport.ini ]] || continue
        cp -r "$item" "$play/"
    done
fi
du -sh "$dest" "dist/$package.zip"
