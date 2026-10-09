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
    'upscaler': 'dlss', 'preset': '1', 'output_res': '1920x1080',
    'dlss_model': 'k', 'live_resolution': '0', 'alpha_detail': '2',
    'model_lod': '-2', 'object_motion': '1', 'sharpen': '1', 'sharpness': '0.30',
    'effect_game_aa': '0',
}
APP = {
    'fps_mode': 'uncap', 'frame_cap': '72', 'present_mode': 'Mailbox',
    'frames_ahead': '1', 'draw_pipe': '', 'readbacks': '',
    'frame_stats': False, 'gpu_profile': False, 'vk_validation': False,
    'check_updates': False,
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
