"""Portable final RTX3060 play profile; optional preserved FG preview, shared saves."""
import argparse,hashlib,inspect,json,os,subprocess
from pathlib import Path

MAIN=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--verify-only',action='store_true')
parser.add_argument('--fg-preview',action='store_true')
args=parser.parse_args()
if not args.verify_only:
    running=subprocess.run(['powershell.exe','-NoProfile','-Command',
        "if (Get-Process -Name 'bb-probe*' -ErrorAction SilentlyContinue) { exit 1 }"],
        stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    if running.returncode:raise SystemExit('Close the running game before starting another session.')
manifest=json.loads((MAIN/'release-manifest.json').read_text(encoding='utf-8'))
mode='fg_preview' if args.fg_preview else 'normal'
if mode not in manifest['renderers']:
    raise SystemExit('This package has no preserved legacy FG preview. Use Play RTX3060.cmd.')
renderer=MAIN/manifest['renderers'][mode]['path']
assert hashlib.sha256(renderer.read_bytes()).hexdigest()==manifest['renderers'][mode]['sha256']
user=MAIN/('optional/fg/user' if args.fg_preview else 'user')
config=MAIN/('optional/fg/bbport.ini' if args.fg_preview else 'bbport.ini')
canonical=MAIN/'user/savedata'
if args.fg_preview:
    if not (user/'savedata').exists():
        canonical.mkdir(parents=True,exist_ok=True);user.mkdir(parents=True,exist_ok=True)
        quote=lambda p:str(p).replace("'","''")
        subprocess.run(['powershell.exe','-NoProfile','-Command',
            f"New-Item -ItemType Junction -Path '{quote(user/'savedata')}' -Target '{quote(canonical)}' | Out-Null"],check=True)
    assert (user/'savedata').is_junction() and (user/'savedata').resolve()==canonical.resolve()
for key in list(os.environ):
    if key.startswith('BB_'):os.environ.pop(key)
os.environ.update(BB_DATA_DIR=str(MAIN),BB_USER_DIR=str(user),BB_GPU_USER_DIR=str(user),
    BB_CONFIG=str(config),BB_PROBE=str(renderer),BB_NGX_DIR=str(MAIN/'bin'))
launcher=next(f.frame.f_globals for f in inspect.stack() if f.function=='run_role')
settings={**launcher['APP_DEFAULTS'],**launcher['load_json'](MAIN/'launcher-settings/bbport-launcher/settings.json',{})}
flags=[f'BB_FRAME_GEN={int(args.fg_preview)}',
    f'BB_OCCLUSION={int(not args.fg_preview)}','BB_BLOOM_OUTLIER_GUARD=2',
    'BB_TEXTURE_UPLOAD_BASELINE=0','BB_OBJECT_MOTION_COMPACT=0','BB_QUIET_GC=1',
    'BB_DEPTH_TILE_FUSION=1','BB_FAST_LOADING=1','BB_TEXTURE_HELPER=0','BB_HOST_COPY_WAITS=all',
    'BB_MULTI_COPY_VECTOR=0','BB_RING_BIND_CURSOR=0','BB_MATERIAL_UBO=0',
    'BB_MATERIAL_EARLY_TESTS=0','BB_CLIP_TEST_FUSION=0','BB_MATERIAL_CSE=0']
if args.fg_preview:
    flags+=['BB_FG_DISPLAY_FORMAT_FIX=1','BB_OBJECT_MOTION_GUARD=1','BB_FG_FI_DIAGNOSTIC=0',
        'BB_FG_FAST_PATH=1','BB_FG_CPU_REUSE=1','BB_FG_GPU_SEARCH=1','BB_FG_GPU_BATCH=0',
        'BB_VBLANK_HZ=0']
settings.update(user_dir=str(user),fps_mode='uncap',frame_cap='72',frames_ahead='1',
    frame_stats=False,gpu_profile=False,vk_validation=False,hdr=False,
    present_mode='Fifo' if args.fg_preview else 'Mailbox',online=False,
    mods_dir=settings.get('mods_dir') or str(MAIN/'mods'),
    patches_dir=settings.get('patches_dir') or str(MAIN/'patches'),extra_env=' '.join(flags))
env=launcher['game_environment'](settings)
os.environ['PATH']=str(MAIN/'bin')+os.pathsep+env.get('PATH','')
for key in ('BB_CAPTURE_DIR','BB_DUMP_DIR','BB_FRAME_PICTURES','BB_BLUR_DIAGNOSTICS',
    'BB_LIGHT_DIAGNOSTICS','BB_FG_MOTION_CAPTURE','BB_SCENE_MOTION_CAPTURE',
    'BB_GPU_PROFILE','BB_CPU_SAMPLE','BB_CPU_DRAW_PROFILE','BB_VK_VALIDATION','BB_GEOMETRY_LOG',
    'BB_CACAO','BB_CACAO_COMPARE','BB_FRAME_STATS'):
    assert key not in env,key
assert env['BB_FPS_LIMIT']=='72' and env['BB_FRAMES_AHEAD']=='1'
assert env['BB_FRAME_GEN']==str(int(args.fg_preview))
record=dict(mode=mode,renderer_sha256=manifest['renderers'][mode]['sha256'],renderer=str(renderer),
    config=str(config),canonical_saves=str(canonical.resolve()),user=str(user),verify_only=args.verify_only,
    captures=False,profilers=False,environment={k:v for k,v in env.items() if k.startswith('BB_')})
user.mkdir(parents=True,exist_ok=True)
(user/('final-verify.json' if args.verify_only else 'final-launch.json')).write_text(json.dumps(record,indent=2),encoding='utf-8')
print('Final RTX3060: '+('preserved experimental FG preview, 72 real target / Fifo' if args.fg_preview else 'accepted classic renderer, OC ON, 72 FPS/Mailbox')+'.',flush=True)
print('Current shared saves; no image capture, extended diagnostics or profilers.',flush=True)
if args.verify_only:
    print('PASS: final renderer, effective environment and shared progress checked; no gameplay launched.',flush=True)
    raise SystemExit(0)
if not settings.get('game_dir'):
    raise SystemExit('Open Bloodborne.exe once and select your decrypted Bloodborne 1.09 game folder, then use Play RTX3060.cmd.')
raise SystemExit(launcher['play_without_window'](settings))
