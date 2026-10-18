#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Defaults for the separately packaged RTX 3060 6 GB Windows build.

Only supplies defaults: saved game paths, saves, mods and settings remain owned by the user.
"""
import argparse
import json
from pathlib import Path

NAME = 'rtx3060-6gb'
INI = {
    'upscaler': 'dlss', 'preset': '5', 'output_res': '1920x1080',
    'dlss_model': 'k', 'live_resolution': '0', 'alpha_detail': '0',
    'model_lod': '0', 'object_motion': '1', 'sharpen': '1', 'sharpness': '0.50',
    'jitter': '0', 'effect_game_aa': '0', 'effect_dof': '0',
    'effect_motion_blur': '0', 'effect_chromatic_aberration': '0',
    'effect_ssao': '1', 'effect_dynamic_shadows': '1', 'effect_ssr': '0',
}
APP = {
    'fps_mode': 'uncap', 'frame_cap': '72', 'present_mode': 'Mailbox',
    'frames_ahead': '1', 'draw_pipe': '', 'readbacks': '',
    'frame_stats': False, 'gpu_profile': False, 'vk_validation': False,
    'check_updates': False,
    'extra_env': 'BB_FRAME_GEN=0 BB_OCCLUSION=1 BB_BLOOM_OUTLIER_GUARD=2 '
                 'BB_TEXTURE_UPLOAD_BASELINE=0 BB_OBJECT_MOTION_COMPACT=0 '
                 'BB_QUIET_GC=1 BB_DEPTH_TILE_FUSION=1 BB_FAST_LOADING=1 '
                 'BB_TEXTURE_HELPER=0 BB_HOST_COPY_WAITS=all BB_MULTI_COPY_VECTOR=0 '
                 'BB_RING_BIND_CURSOR=0 BB_MATERIAL_UBO=0 BB_MATERIAL_EARLY_TESTS=0 '
                 'BB_CLIP_TEST_FUSION=0 BB_MATERIAL_CSE=0',
}
ENVIRONMENT = {'BB_GC_IDLE_SECONDS': '20', 'BB_FPS_LIMIT': '72',
               'BB_IDLE_MEMORY_GC': '1', 'BB_QUIET_GC': '1',
               'BB_STAGING_KEEP_MB': '1024', 'BB_STAGING_PREWARM_MB': '256'}


def load_profile(folder):
    try:
        profile = json.loads((Path(folder) / 'windows-profile.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    if not isinstance(profile, dict) or profile.get('name') != NAME:
        return {}
    return {'name': NAME, 'minimal': profile.get('minimal') is True,
            'ini': dict(INI), 'app': dict(APP), 'environment': dict(ENVIRONMENT)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', type=Path, required=True)
    parser.add_argument('--minimal', action='store_true')
    args = parser.parse_args()
    args.write.write_text(json.dumps({'name': NAME, 'minimal': args.minimal}, indent=2) + '\n',
                          encoding='utf-8')


if __name__ == '__main__':
    main()
