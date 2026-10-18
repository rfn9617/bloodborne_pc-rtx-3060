"""Identify a newly built package; do not pretend it is the historical accepted EXE."""
import hashlib,json,subprocess,sys
from pathlib import Path
folder=Path(sys.argv[1])
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
renderers={'normal':dict(path='bin/bb-probe.exe',sha256=sha(folder/'bin/bb-probe.exe'))}
if (folder/'bin/bb-probe-fg-preview.exe').is_file():
    renderers['fg_preview']=dict(path='bin/bb-probe-fg-preview.exe',sha256=sha(folder/'bin/bb-probe-fg-preview.exe'),default_enabled=False)
try:commit=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
except (OSError,subprocess.CalledProcessError):commit=None
(folder/'release-manifest.json').write_text(json.dumps(dict(source_commit=commit,renderers=renderers,
    historical_accepted_renderer_sha256='4d26fd1449ca9f887aeafd0feacb4a3c3d2bcb667b125cb0b0356332176014ed',
    locally_rebuilt=True,game_files_included=False,saves_included=False,caches_included=False),indent=2),encoding='utf-8')
