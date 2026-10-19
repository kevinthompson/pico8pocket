"""Exercise runtime refreshes and complete releases without private cartridges."""
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import hashlib
import zipfile
import subprocess
import io
from contextlib import redirect_stdout
from unittest.mock import patch

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


class RuntimePreparationTests(unittest.TestCase):
    """Exercise real Git tag resolution and fail-closed candidate validation."""

    def git(self, source, *args, input=None):
        """Run offline Git operations on isolated fixture objects only."""
        return subprocess.run(['git', '-C', str(source), *args], input=input,
                              text=True, capture_output=True, check=True).stdout.strip()

    def snapshot(self, source, message):
        """Write synthetic Git commit objects used only as isolated fixture data."""
        self.git(source, 'add', '.')
        tree = self.git(source, 'write-tree')
        data = (f'tree {tree}\n'
                'author Fixture <fixture@example.invalid> 1 +0000\n'
                'committer Fixture <fixture@example.invalid> 1 +0000\n\n'
                f'{message}\n')
        commit = self.git(source, 'hash-object', '-t', 'commit', '-w', '--stdin', input=data)
        self.git(source, 'update-ref', 'refs/heads/fixture', commit)
        self.git(source, 'symbolic-ref', 'HEAD', 'refs/heads/fixture')
        return commit

    def setUp(self):
        """Create clean runtime/release repositories and a three-file os20 bundle."""
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.source = base / 'source'
        self.root = base / 'release'
        self.output = base / 'report'
        self.runtime = base / 'runtime'
        self.api = self.source / 'src/firmware/api'
        self.api.mkdir(parents=True)
        (self.api / 'of_version.h').write_text('#define OF_API_VERSION_MAJOR 0\n#define OF_API_VERSION_MINOR 9\n')
        (self.api / 'of_savestate.h').write_text('/* save-state fixture */\n')
        self.git(self.source, 'init', '--quiet')
        self.commit = self.snapshot(self.source, 'runtime fixture')
        self.tag = 'fixture-lightweight'
        self.git(self.source, 'update-ref', 'refs/tags/' + self.tag, self.commit)
        (self.root / 'scripts').mkdir(parents=True)
        (self.root / 'docs').mkdir()
        script = (ROOT / 'scripts/fetch-deps.sh').read_text()
        old = release.read_pins(script)
        script = script.replace(old['FORK_COMMIT'], self.commit)
        script = script.replace(old['RUNTIME_URL'],
                                'https://github.com/zenibako/openfpgaCore/releases/download/pocket-runtime-old')
        (self.root / 'scripts/fetch-deps.sh').write_text(script)
        for name in ('README.md', 'docs/architecture.md', 'THIRD_PARTY_NOTICES.md'):
            (self.root / name).write_text(f'Runtime `{self.commit}` (`{self.commit[:7]}`), pocket-runtime-old\n')
        self.git(self.root, 'init', '--quiet')
        self.snapshot(self.root, 'release fixture')
        self.before = {name: (self.root / name).read_bytes() for name in
                       ('scripts/fetch-deps.sh', 'README.md', 'docs/architecture.md', 'THIRD_PARTY_NOTICES.md')}
        self.runtime.mkdir()
        for name in ('os20.rbf_r', 'os.bin', 'loader.bin'):
            (self.runtime / name).write_bytes(b'matched fixture ' + name.encode())

    def prepare(self):
        """Run the actual helper against fixtures with application disabled."""
        with patch.object(release, 'ROOT', self.root), redirect_stdout(io.StringIO()):
            release.prepare(self.source, self.runtime, self.tag, self.output, apply=False)

    def assert_candidate(self):
        """Require a usable patch/report while leaving all release inputs intact."""
        self.prepare()
        self.assertIn(f'Candidate: `{self.commit}`', (self.output / 'REVIEW.md').read_text())
        self.git(self.root, 'apply', '--check', str(self.output / 'runtime-pin.patch'))
        self.assertTrue((self.output / 'runtime-pin.patch').stat().st_size)
        for name, original in self.before.items():
            self.assertEqual((self.root / name).read_bytes(), original)

    def test_lightweight_tag_at_head_is_accepted_without_applying_patch(self):
        """Accept an ordinary tag and produce a patch that Git can apply."""
        self.assert_candidate()

    def test_annotated_tag_at_head_is_peeled_to_its_commit(self):
        """Accept annotated tags rather than comparing their tag-object SHA to HEAD."""
        self.tag = 'fixture-annotated'
        data = (f'object {self.commit}\ntype commit\ntag {self.tag}\n'
                'tagger Fixture <fixture@example.invalid> 1 +0000\n\nAnnotated tag fixture\n')
        tag_object = self.git(self.source, 'hash-object', '-t', 'tag', '-w', '--stdin', input=data)
        self.git(self.source, 'update-ref', 'refs/tags/' + self.tag, tag_object)
        self.assertNotEqual(tag_object, self.commit)
        self.assert_candidate()

    def test_tag_at_another_commit_is_rejected_before_writing_output(self):
        """Reject a valid tag when the checkout has moved to a different commit."""
        self.snapshot(self.source, 'different runtime fixture')
        with self.assertRaisesRegex(ValueError, 'does not match'):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_dirty_checkout_is_rejected_before_writing_output(self):
        """Reject both modified tracked headers and untracked source files."""
        for path in (self.api / 'of_version.h', self.source / 'untracked.txt'):
            with self.subTest(path=path.name):
                original = path.read_bytes() if path.exists() else None
                path.write_text('dirty fixture\n')
                with self.assertRaisesRegex(ValueError, 'clean, dedicated checkout'):
                    self.prepare()
                self.assertFalse(self.output.exists())
                if original is None:
                    path.unlink()
                else:
                    path.write_bytes(original)

    def test_incompatible_api_is_rejected_even_in_a_clean_tagged_checkout(self):
        """Do not accept a matched source tag with an unsupported API version."""
        (self.api / 'of_version.h').write_text('#define OF_API_VERSION_MAJOR 0\n#define OF_API_VERSION_MINOR 8\n')
        self.commit = self.snapshot(self.source, 'incompatible API fixture')
        self.git(self.source, 'update-ref', 'refs/tags/' + self.tag, self.commit)
        with self.assertRaisesRegex(ValueError, 'API must match'):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_missing_runtime_file_is_rejected_before_writing_output(self):
        """Require all three published files before preparing a runtime patch."""
        (self.runtime / 'loader.bin').unlink()
        with self.assertRaisesRegex(ValueError, 'loader.bin'):
            self.prepare()
        self.assertFalse(self.output.exists())


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
