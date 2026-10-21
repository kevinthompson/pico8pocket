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
import tarfile
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

    def tool_assets(self, directory=None):
        """Use the independently specified public offline plugin payload."""
        directory = directory or self.root
        for name in ('OFFLINE-COLLECTIONS.md', 'LICENSE.txt', 'TOOL-NOTICES.txt'):
            (directory / name).write_text('public fixture\n')
        (directory / 'plugin.wasm').write_bytes(b'\x00asm\x01\x00\x00\x00')
        manifest = {'name': 'Pico8Pocket Offline Collections',
                    'description': 'Offline collection fixture',
                    'allowed_hosts': ['www.lexaloffle.com']}
        (directory / 'plugin.json').write_text(json.dumps(manifest))
        return manifest

    def test_tool_assets_must_be_complete_and_not_contain_private_extras(self):
        manifest = self.tool_assets()
        with tempfile.TemporaryDirectory() as source:
            source = Path(source)
            (source / 'plugin.json').write_text(json.dumps(manifest))
            with patch.object(release, 'ROOT', source):
                release.validate_tools(self.root)
                for extra in ('cart.p8', 'bbs.p8pack', 'cache.json', 'cart-thumbnails.wasm',
                              'THUMBNAILS.md', *[f'cart-thumbnails-{system}-{arch}.tar.gz'
                              for system in ('darwin', 'linux', 'windows') for arch in ('amd64', 'arm64')]):
                    with self.subTest(extra=extra):
                        (self.root / extra).write_bytes(b'private or retired')
                        with self.assertRaisesRegex(ValueError, 'assets'):
                            release.validate_tools(self.root)
                        (self.root / extra).unlink()
                (self.root / 'plugin.wasm').unlink()
                with self.assertRaisesRegex(ValueError, 'assets'):
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
        for name in ('Assets/pico8/common/cards/private.p8', 'Saves/progress.sav', '../private', 'Assets/pico8/common/cards/unknown.bin',
                     'Assets/pico8/common/cards/bbs.p8pack',
                     'Assets/pico8/common/cards/cache/verified-cart.bin',
                     'Assets/pico8/common/cards/cache/index.json'):
            with self.subTest(name=name):
                core, pins = self.core_zip({name: b'private'})
                with self.assertRaises(ValueError):
                    release.core_files(core, pins)


class DistributionAssemblyTests(unittest.TestCase):
    """Exercise the real release assembler, Git archive and provenance checks offline."""

    git = RuntimePreparationTests.git
    snapshot = RuntimePreparationTests.snapshot
    tool_assets = DistributionTests.tool_assets
    core_zip = DistributionTests.core_zip

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.root = base / 'source'
        self.root.mkdir()
        self.tools = base / 'tools'
        self.tools.mkdir()
        self.manifest = self.tool_assets(self.tools)
        (self.root / 'plugin.json').write_text(json.dumps(self.manifest))
        self.core_dir = base / 'core'
        self.core_dir.mkdir()
        core, pins = self.core_zip()
        self.core_name = 'pico8pocket-v1.2.3.zip'
        core.rename(self.core_dir / self.core_name)
        (self.root / 'dist/core').mkdir(parents=True)
        (self.root / 'dist/core/core.json').write_text(json.dumps({'core': {'metadata': {'version': '1.2.3'}}}))
        (self.root / 'scripts').mkdir()
        script = (ROOT / 'scripts/fetch-deps.sh').read_text()
        old = release.read_pins(script)
        for name, old_hash in old['runtime_hashes'].items():
            script = script.replace(old_hash, pins['runtime_hashes'][name])
        (self.root / 'scripts/fetch-deps.sh').write_text(script)
        self.git(self.root, 'init', '--quiet')
        self.commit = self.snapshot(self.root, 'public release fixture')
        self.output = base / 'distribution'
        self.root_patch = patch.object(release, 'ROOT', self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        release.provenance(self.core_dir / self.core_name,
                           self.core_dir / 'CORE-PROVENANCE.json', '13.2.0')

    def assemble(self):
        release.assemble(self.core_dir, self.tools, self.output)

    def test_assembly_contains_only_offline_plugin_core_source_and_verified_checksums(self):
        (self.tools / 'SHA256SUMS').write_text('obsolete artifact checksum\n')
        self.assemble()
        self.assertEqual({p.name for p in self.output.iterdir()}, {
            'plugin.wasm', 'plugin.json', 'OFFLINE-COLLECTIONS.md', 'LICENSE.txt',
            'TOOL-NOTICES.txt', 'pico8pocket-v1.2.3.zip', 'CORE-PROVENANCE.json',
            'pico8pocket-source.tar.gz', 'SHA256SUMS'})
        self.assertEqual(json.loads((self.output / 'plugin.json').read_text()), self.manifest)
        with tarfile.open(self.output / 'pico8pocket-source.tar.gz') as archive:
            self.assertEqual(archive.extractfile('plugin.json').read(),
                             (self.root / 'plugin.json').read_bytes())
        sums = (self.output / 'SHA256SUMS').read_text().splitlines()
        self.assertEqual(len(sums), 8)
        self.assertEqual([line.split('  ')[1] for line in sums], sorted(
            ['plugin.wasm', 'plugin.json', 'OFFLINE-COLLECTIONS.md', 'LICENSE.txt',
             'TOOL-NOTICES.txt', self.core_name, 'CORE-PROVENANCE.json', 'pico8pocket-source.tar.gz']))
        for line in sums:
            checksum, name = line.split('  ')
            self.assertEqual(checksum, hashlib.sha256((self.output / name).read_bytes()).hexdigest())

    def test_invalid_wasm_is_rejected_before_creating_distribution(self):
        (self.tools / 'plugin.wasm').write_bytes(b'not a wasm module')
        with self.assertRaisesRegex(ValueError, 'WASM'):
            self.assemble()
        self.assertFalse(self.output.exists())

    def test_offline_manifest_requires_canonical_identity_and_allowed_host(self):
        for changes in ({'name': 'Pico8Pocket Cart Thumbnails'}, {'allowed_hosts': []},
                        {'allowed_hosts': ['www.lexaloffle.com', 'untrusted.invalid']},
                        {'description': 'stale metadata'}):
            with self.subTest(changes=changes):
                (self.tools / 'plugin.json').write_text(json.dumps(self.manifest | changes))
                with self.assertRaisesRegex(ValueError, 'manifest'):
                    self.assemble()
                self.assertFalse(self.output.exists())

    def test_malformed_or_nonobject_manifest_prevents_assembly(self):
        for manifest in ('{not json', '[]', 'null'):
            with self.subTest(manifest=manifest):
                (self.tools / 'plugin.json').write_text(manifest)
                with self.assertRaisesRegex(ValueError, 'manifest'):
                    self.assemble()
                self.assertFalse(self.output.exists())

    def test_nonflat_or_symlinked_plugin_assets_prevent_assembly(self):
        nested = self.tools / 'cache'
        nested.mkdir()
        with self.assertRaisesRegex(ValueError, 'assets'):
            self.assemble()
        nested.rmdir()
        wasm = self.tools / 'plugin.wasm'
        wasm.unlink()
        target = self.tools.parent / 'plugin.wasm'
        target.write_bytes(b'\x00asm\x01\x00\x00\x00')
        wasm.symlink_to(target)
        with self.assertRaisesRegex(ValueError, 'assets'):
            self.assemble()
        self.assertFalse(self.output.exists())

    def test_canonical_manifest_cannot_authorize_retired_identity_or_extra_hosts(self):
        for changes in ({'name': 'Pico8Pocket Cart Thumbnails'},
                        {'allowed_hosts': ['www.lexaloffle.com', 'untrusted.invalid']}):
            with self.subTest(changes=changes):
                manifest = json.dumps(self.manifest | changes)
                (self.root / 'plugin.json').write_text(manifest)
                (self.tools / 'plugin.json').write_text(manifest)
                self.snapshot(self.root, 'invalid canonical manifest fixture')
                with self.assertRaisesRegex(ValueError, 'manifest'):
                    self.assemble()
                self.assertFalse(self.output.exists())

    def test_private_extra_or_retired_thumbnail_asset_prevents_assembly(self):
        for name in ('bbs.p8pack', 'cart.p8', 'cache.json', 'cart-thumbnails.wasm',
                     'cart-thumbnails-linux-amd64.tar.gz', 'THUMBNAILS.md'):
            with self.subTest(name=name):
                path = self.tools / name
                path.write_bytes(b'not public')
                with self.assertRaisesRegex(ValueError, 'assets'):
                    self.assemble()
                self.assertFalse(self.output.exists())
                path.unlink()

    def test_provenance_must_match_source_pins_compiler_and_core_files(self):
        path = self.core_dir / 'CORE-PROVENANCE.json'
        record = json.loads(path.read_text())
        for key, value in (('source_commit', '0' * 40), ('pins', {}),
                           ('compiler', '12.2.0'), ('files', {})):
            with self.subTest(key=key):
                path.write_text(json.dumps(record | {key: value}))
                with self.assertRaisesRegex(ValueError, 'provenance'):
                    self.assemble()
                self.assertFalse(self.output.exists())

    def test_dirty_source_prevents_assembly(self):
        (self.root / 'private.p8').write_text('private')
        with self.assertRaisesRegex(ValueError, 'committed and clean'):
            self.assemble()
        self.assertFalse(self.output.exists())

    def test_extra_core_artifact_prevents_assembly(self):
        (self.core_dir / 'private.p8').write_text('private')
        with self.assertRaisesRegex(ValueError, 'core artifacts'):
            self.assemble()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
