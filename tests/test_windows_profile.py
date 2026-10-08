from paths import ROOT
import importlib.util
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from windows_profile import APP, INI, NAME, load_profile


def launcher():
    spec = importlib.util.spec_from_file_location('windows_launcher_test',
                                                ROOT / 'launcher/bbport_launcher_win.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WindowsProfileTests(unittest.TestCase):
    def test_profile_generation_and_minimal_defaults(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / 'windows-profile.json'
            result = subprocess.run([os.sys.executable, str(ROOT / 'scripts/windows_profile.py'),
                                     '--write', str(target), '--minimal'], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            profile = load_profile(folder)
        self.assertEqual(profile['ini']['upscaler'], 'dlss')
        self.assertEqual(profile['ini']['preset'], '1')
        self.assertEqual(profile['ini']['output_res'], '1920x1080')
        self.assertEqual(profile['ini']['dlss_model'], 'k')
        self.assertEqual(profile['app']['frame_cap'], '72')
        self.assertTrue(profile['minimal'])
        self.assertEqual(profile['environment']['BB_GC_IDLE_SECONDS'], '20')
        self.assertEqual(profile['environment']['BB_STAGING_KEEP_MB'], '1024')
        self.assertEqual(profile['environment']['BB_STAGING_PREWARM_MB'], '256')
        self.assertLessEqual(int(profile['app']['frame_cap']), 120)

    def test_unknown_and_invalid_profiles_do_not_change_other_builds(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(load_profile(folder), {})
            file = Path(folder) / 'windows-profile.json'
            for data in ('broken', '[]', '{}', '{"name":"unknown"}'):
                file.write_text(data)
                self.assertEqual(load_profile(folder), {})

    def test_saved_paths_and_settings_override_profile_defaults(self):
        module = launcher()
        with tempfile.TemporaryDirectory() as folder:
            config = Path(folder) / 'bbport.ini'
            config.write_text('# keep me\nupscaler=taa\nmodel_lod=0\ncustom=keep\n')
            with patch.object(module, 'INI_DEFAULTS', {**module.INI_DEFAULTS, **INI}), \
                    patch.dict(os.environ, {'BB_CONFIG': str(config)}):
                values, lines = module.load_ini()
                self.assertEqual(values['upscaler'], 'taa')
                self.assertEqual(values['model_lod'], '0')
                self.assertEqual(values['alpha_detail'], '2')
                module.save_ini(values, lines)
            self.assertIn('# keep me\n', config.read_text())
            self.assertIn('custom=keep\n', config.read_text())
        settings = {**module.APP_DEFAULTS, **APP, 'game_dir': 'D:/My Games/CUSA03173',
                    'user_dir': 'D:/My Saves', 'frame_cap': '90'}
        with patch.object(module, 'WINDOWS_PROFILE', {'environment': {'BB_GC_IDLE_SECONDS': '5'}}):
            env = module.game_environment(settings)
        self.assertEqual(env['BB_GAME_DIR'], 'D:/My Games/CUSA03173')
        self.assertEqual(env['BB_USER_DIR'], 'D:/My Saves')
        self.assertEqual(env['BB_FPS_LIMIT'], '90')

    def test_disabling_diagnostics_removes_inherited_flags_and_extra_env_wins(self):
        module = launcher()
        keys = ('BB_FRAME_STATS', 'BB_GPU_PROFILE', 'BB_VK_VALIDATION')
        with patch.dict(os.environ, dict.fromkeys(keys, '1')):
            env = module.game_environment({**module.APP_DEFAULTS, **APP})
            for key in keys:
                self.assertNotIn(key, env)
            env = module.game_environment({**module.APP_DEFAULTS, **APP,
                                           'extra_env': 'BB_FRAME_STATS=1 BB_GC_IDLE_SECONDS=12'})
            self.assertEqual(env['BB_FRAME_STATS'], '1')
            self.assertEqual(env['BB_GC_IDLE_SECONDS'], '12')

    def test_loader_returns_independent_defaults(self):
        with tempfile.TemporaryDirectory() as folder:
            Path(folder, 'windows-profile.json').write_text(json.dumps({'name': NAME}))
            profile = load_profile(folder)
            profile['ini']['upscaler'] = 'off'
            self.assertEqual(load_profile(folder)['ini']['upscaler'], 'dlss')

    def test_minimal_package_rejects_missing_profile_before_building(self):
        result = subprocess.run(['bash', 'packaging/windows/package.sh', '--minimal'],
                                cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('--minimal requires --profile', result.stderr)

    def test_dlss_profile_requires_runtime_and_license(self):
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder, 'packaging/windows/package.sh')
            script.parent.mkdir(parents=True)
            script.write_bytes((ROOT / 'packaging/windows/package.sh').read_bytes())
            result = subprocess.run(['bash', str(script), '--profile', NAME, '--minimal'],
                                    capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('build_dlss.sh first', result.stderr)
