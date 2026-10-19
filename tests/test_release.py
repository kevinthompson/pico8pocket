"""Exercise runtime refreshes and complete releases without private cartridges."""
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import hashlib
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release', ROOT / 'scripts/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class RuntimeUpdateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for name in ('scripts/fetch-deps.sh', 'README.md', 'docs/architecture.md', 'THIRD_PARTY_NOTICES.md'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, path)
        self.script = (self.root / 'scripts/fetch-deps.sh').read_text()
        self.old = release.read_pins(self.script)
        self.hashes = {name: '2' * 64 for name in ('os20.rbf_r', 'os.bin', 'loader.bin')}

    def test_refresh_keeps_sdk_and_vm_pins_and_updates_all_runtime_references(self):
        changes = release.runtime_changes(self.root, '1' * 40, 'pocket-runtime-test', self.hashes)
        pins = release.read_pins(changes['scripts/fetch-deps.sh'])
        self.assertEqual(pins['FORK_COMMIT'], '1' * 40)
        self.assertEqual(pins['runtime_hashes'], self.hashes)
        self.assertEqual(pins['RUNTIME_URL'], 'https://github.com/zenibako/openfpgaCore/releases/download/pocket-runtime-test')
        for key in ('SDK_COMMIT', 'DIABLO_COMMIT', 'FAKE08_COMMIT', 'Z8LUA_COMMIT'):
            self.assertEqual(pins[key], self.old[key])
        for name in ('README.md', 'docs/architecture.md', 'THIRD_PARTY_NOTICES.md'):
            self.assertIn('pocket-runtime-test', changes[name])
            self.assertNotIn(self.old['RUNTIME_URL'].rsplit('/', 1)[1], changes[name])
        self.assertEqual((self.root / 'scripts/fetch-deps.sh').read_text(), self.script)

    def test_current_bundle_is_noop(self):
        self.assertEqual(release.runtime_changes(self.root, self.old['FORK_COMMIT'],
                         self.old['RUNTIME_URL'].rsplit('/', 1)[1], self.old['runtime_hashes']), {})

    def test_wrong_variant_or_incomplete_hashes_cannot_update(self):
        for hashes in ({'os25.rbf_r': '2' * 64}, {'os20.rbf_r': 'bad', 'os.bin': '2' * 64, 'loader.bin': '2' * 64}):
            with self.assertRaises(ValueError):
                release.runtime_changes(self.root, '1' * 40, 'pocket-runtime-test', hashes)

    def test_floating_commits_and_unsafe_tags_are_rejected(self):
        for commit, tag in [('main', 'test'), ('1' * 40, '../test'), ('1' * 40, 'test\n')]:
            with self.assertRaises(ValueError):
                release.runtime_changes(self.root, commit, tag, self.hashes)

    def test_documentation_drift_prevents_partial_update(self):
        (self.root / 'docs/architecture.md').write_text('Unreconciled documentation')
        with self.assertRaisesRegex(ValueError, 'architecture.md'):
            release.runtime_changes(self.root, '1' * 40, 'pocket-runtime-test', self.hashes)
        self.assertEqual((self.root / 'scripts/fetch-deps.sh').read_text(), self.script)


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.pins = release.read_pins((ROOT / 'scripts/fetch-deps.sh').read_text())

    def test_tool_assets_must_be_complete_and_not_contain_private_extras(self):
        for name in release.TOOL_ASSETS:
            (self.root / name).write_bytes(b'fixture')
        release.validate_tools(self.root)
        (self.root / 'cart.p8').write_text('private')
        with self.assertRaises(ValueError):
            release.validate_tools(self.root)
        (self.root / 'cart.p8').unlink()
        (self.root / 'plugin.wasm').unlink()
        with self.assertRaises(ValueError):
            release.validate_tools(self.root)

    def test_checksum_manifest_covers_core_and_tools_but_never_itself(self):
        (self.root / 'core.zip').write_bytes(b'core')
        (self.root / 'plugin.wasm').write_bytes(b'plugin')
        (self.root / 'SHA256SUMS').write_text('obsolete tools-only manifest')
        release.checksums(self.root)
        text = (self.root / 'SHA256SUMS').read_text()
        self.assertEqual(len(text.splitlines()), 2)
        self.assertIn('  core.zip\n', text)
        self.assertIn('  plugin.wasm\n', text)
        self.assertNotIn('SHA256SUMS', text)

    def core_zip(self, extra=None, corrupt_runtime=False):
        files = {'Cores/zenibako.pico8/core.json': json.dumps({'core': {'cores': [{'filename': 'os20.rbf_r'}]}}).encode(),
                 'Cores/zenibako.pico8/os20.rbf_r': b'bitstream',
                 'Cores/zenibako.pico8/loader.bin': b'loader',
                 'Assets/pico8/common/os.bin': b'firmware',
                 'Assets/pico8/common/pico8pocket.elf': b'\x7fELF\x01\x01' + bytes(12) + b'\xf3\x00' + bytes(32)}
        pins = {'runtime_hashes': dict(zip(('os20.rbf_r', 'loader.bin', 'os.bin'),
                [hashlib.sha256(data).hexdigest() for data in (b'bitstream', b'loader', b'firmware')]))}
        if corrupt_runtime:
            files['Assets/pico8/common/os.bin'] = b'wrong runtime'
        files.update(extra or {})
        path = self.root / 'core.zip'
        with zipfile.ZipFile(path, 'w') as archive:
            for name, data in files.items():
                archive.writestr(name, data)
        return path, pins

    def test_core_archive_preserves_pinned_runtime_identity(self):
        core, pins = self.core_zip()
        files = release.core_files(core, pins)
        self.assertEqual(len(files), 5)
        self.assertEqual(files['Assets/pico8/common/os.bin'], hashlib.sha256(b'firmware').hexdigest())
        core, pins = self.core_zip(corrupt_runtime=True)
        with self.assertRaisesRegex(ValueError, 'runtime'):
            release.core_files(core, pins)

    def test_private_or_escaping_archive_entries_cannot_be_released(self):
        for name in ('Assets/pico8/common/cards/private.p8', 'Saves/progress.sav', '../private', 'Assets/pico8/common/cards/unknown.bin'):
            with self.subTest(name=name):
                core, pins = self.core_zip({name: b'private'})
                with self.assertRaises(ValueError):
                    release.core_files(core, pins)


if __name__ == '__main__':
    unittest.main()
