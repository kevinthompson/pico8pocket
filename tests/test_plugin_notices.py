"""Exercise plugin redistribution notices using project-owned license fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/plugin-notices.py'
spec = importlib.util.spec_from_file_location('plugin_notices', SCRIPT)
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


class PluginNoticesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.output = self.root / 'TOOL-NOTICES.txt'
        self.sysroot = self.root / 'sysroot'
        self.rustdoc = self.sysroot / 'share/doc/rust'
        (self.rustdoc / 'licenses').mkdir(parents=True)
        (self.rustdoc / 'COPYRIGHT-library.html').write_bytes(b'<html>Fixture standard library notice</html>\n')
        (self.rustdoc / 'licenses/MIT.txt').write_bytes(b'Fixture Rust MIT permission\nAll rights retained.\n')
        self.packages = []
        self.nodes = []
        self.package('plugin', [], source=None)
        self.package('z-runtime', [('nested', None)], files={'LICENSE-MIT': b'Fixture MIT full text\r\nLine two\r\n', 'NOTICE': b'Fixture contributor notice\n'})
        self.package('a-build', [], files={'licenses/Apache-2.0.txt': b'Fixture complete Apache terms\n'})
        self.package('nested', [], files={'terms.txt': b'Fixture custom license\n'}, license_file='terms.txt')
        self.package('dev-only', [('dev-child', None)])
        self.package('dev-child', [])
        self.package('unreachable', [])
        self.nodes[0]['deps'] = [self.edge('z-runtime'), self.edge('a-build', 'build'), self.edge('dev-only', 'dev')]
        self.metadata = {'packages': self.packages, 'resolve': {'root': 'plugin', 'nodes': self.nodes}}
        self.metadata_file = self.root / 'metadata.json'
        self.cargo = self.root / 'fixture-cargo'
        self.cargo.write_text('#!' + sys.executable + "\nimport json,os,sys\nfrom pathlib import Path\nassert sys.argv[1:]==['metadata','--locked','--format-version','1','--filter-platform','wasm32-wasip1','--manifest-path',os.environ['FIXTURE_MANIFEST']]\nprint(Path(os.environ['FIXTURE_METADATA']).read_text())\n")
        self.cargo.chmod(0o755)
        self.rustc = self.root / 'fixture-rustc'
        self.rustc.write_text('#!' + sys.executable + "\nimport os,sys\nif sys.argv[1:]==['--print','sysroot']: print(os.environ['FIXTURE_SYSROOT'])\nelif sys.argv[1:]==['--version']: print('rustc 1.93.0 (fixture)')\nelse: sys.exit(9)\n")
        self.rustc.chmod(0o755)
        self.env = dict(os.environ, CARGO=str(self.cargo), RUSTC=str(self.rustc),
                        FIXTURE_METADATA=str(self.metadata_file), FIXTURE_SYSROOT=str(self.sysroot),
                        FIXTURE_MANIFEST=str(ROOT / 'tools/pocket-sync/Cargo.toml'))

    def edge(self, name, kind=None):
        return {'name': name, 'pkg': name, 'dep_kinds': [{'kind': kind, 'target': None}]}

    def package(self, name, deps, files=None, source='registry+fixture', license_file=None):
        directory = self.root / 'crates' / name
        directory.mkdir(parents=True)
        (directory / 'Cargo.toml').write_text('[package]\n')
        for filename, text in (files or {}).items():
            path = directory / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text)
        self.packages.append({'id': name, 'name': name, 'version': '1.0.0', 'source': source,
                              'manifest_path': str(directory / 'Cargo.toml'),
                              'license': 'MIT', 'license_file': license_file})
        self.nodes.append({'id': name, 'deps': [self.edge(n, k) for n, k in deps]})

    def run_notices(self):
        self.metadata_file.write_text(json.dumps(self.metadata))
        return subprocess.run([sys.executable, str(SCRIPT), str(self.output)], env=self.env,
                              text=True, capture_output=True)

    def test_reachable_runtime_build_and_nested_licenses_are_complete_and_sorted(self):
        result = self.run_notices()
        self.assertEqual(result.returncode, 0, result.stderr)
        data = self.output.read_bytes()
        for text in (b'Fixture MIT full text\r\nLine two\r\n', b'Fixture contributor notice\n',
                     b'Fixture complete Apache terms\n', b'Fixture custom license\n',
                     b'<html>Fixture standard library notice</html>\n',
                     b'Fixture Rust MIT permission\nAll rights retained.\n'):
            self.assertIn(text, data)
        self.assertLess(data.index(b'a-build 1.0.0'), data.index(b'nested 1.0.0'))
        self.assertLess(data.index(b'nested 1.0.0'), data.index(b'z-runtime 1.0.0'))
        for name in (b'dev-only', b'dev-child', b'unreachable', str(self.root).encode()):
            self.assertNotIn(name, data)
        self.metadata['packages'].reverse()
        self.metadata['resolve']['nodes'].reverse()
        result = self.run_notices()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_bytes(), data)

    def test_pinned_extism_license_fallback_is_included(self):
        self.package('extism-convert', [])
        package = self.packages[-1]
        package.update(version='1.41.0', license='BSD-3-Clause',
                       repository='https://github.com/extism/extism')
        directory = Path(package['manifest_path']).parent
        (directory / '.cargo_vcs_info.json').write_text(json.dumps({
            'git': {'sha1': '051b765bb6abe4fe95d5e4f489f03ad7b137edbc'},
            'path_in_vcs': 'convert'}))
        self.nodes[0]['deps'].append(self.edge('extism-convert'))
        result = self.run_notices()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'extism-convert 1.41.0', self.output.read_bytes())
        self.assertIn(b'Copyright 2022 Dylibso, Inc.', self.output.read_bytes())

    def test_absolute_declared_license_path_does_not_leak_checkout_path(self):
        package = next(p for p in self.packages if p['id'] == 'nested')
        package['license'] = None
        package['license_file'] = str(self.root / 'crates/nested/terms.txt')
        result = self.run_notices()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'license_file: terms.txt', self.output.read_bytes())
        self.assertNotIn(str(self.root).encode(), self.output.read_bytes())

    def add_extism(self):
        self.package('extism-manifest', [])
        package = self.packages[-1]
        package.update(version='1.41.0', license='BSD-3-Clause',
                       repository='https://github.com/extism/extism')
        vcs = Path(package['manifest_path']).parent / '.cargo_vcs_info.json'
        vcs.write_text(json.dumps({'git': {'sha1': '051b765bb6abe4fe95d5e4f489f03ad7b137edbc'}}))
        self.nodes[0]['deps'].append(self.edge('extism-manifest'))
        return package, vcs

    def test_supplement_uses_complete_fixture_text(self):
        self.add_extism()
        directory = self.root / 'supplemental'
        directory.mkdir()
        text = b'Fixture complete BSD text\r\nRedistribution terms and disclaimer.\r\n'
        (directory / 'extism-1.41.0.txt').write_bytes(text)
        data = notices.generate_notices(self.metadata, self.sysroot, 'rustc 1.93.0 fixture', directory)
        self.assertIn(text, data)
        self.assertNotIn(str(self.root).encode(), data)

    def test_unknown_supplement_version_repository_or_revision_preserves_output(self):
        package, vcs = self.add_extism()
        original = package.copy()
        for change in ({'version': '1.41.1'}, {'repository': 'https://example.invalid/fork'},
                       {'license': 'MIT'}, {'revision': '0' * 40}):
            with self.subTest(change=change):
                package.clear()
                package.update(original)
                package.update({key: value for key, value in change.items() if key != 'revision'})
                vcs.write_text(json.dumps({'git': {'sha1': change.get('revision',
                    '051b765bb6abe4fe95d5e4f489f03ad7b137edbc')}}))
                self.output.write_bytes(b'previous valid notices')
                result = self.run_notices()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('extism-manifest', result.stderr)
                self.assertEqual(self.output.read_bytes(), b'previous valid notices')

    def test_missing_dependency_license_fails_without_replacing_output(self):
        (self.root / 'crates/z-runtime/LICENSE-MIT').unlink()
        self.output.write_text('previous valid notices')
        result = self.run_notices()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('z-runtime', result.stderr)
        self.assertEqual(self.output.read_text(), 'previous valid notices')

    def test_missing_declared_license_file_fails(self):
        (self.root / 'crates/nested/terms.txt').unlink()
        result = self.run_notices()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('nested', result.stderr)

    def test_empty_license_fails(self):
        (self.root / 'crates/nested/terms.txt').write_bytes(b'')
        result = self.run_notices()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('nested', result.stderr)

    def test_missing_standard_library_notices_fail(self):
        (self.rustdoc / 'COPYRIGHT-library.html').unlink()
        result = self.run_notices()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('COPYRIGHT-library.html', result.stderr)

    def test_incomplete_resolve_graph_fails(self):
        self.nodes[:] = [node for node in self.nodes if node['id'] != 'nested']
        result = self.run_notices()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('nested', result.stderr)


if __name__ == '__main__':
    unittest.main()
