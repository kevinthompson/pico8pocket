"""The installer catalog must produce a checked core dependency table."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/offline-carts.py'

class OfflineCatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'compatibility').mkdir()
        (self.root / 'include/p8p').mkdir(parents=True)
        self.path = self.root / 'compatibility/offline-carts.json'
        self.catalog = {'version': 1, 'carts': [
            {'id': '62288', 'name': 'Pzone', 'collection': 'arcade'},
            {'id': 'mot_sorcerergame', 'name': 'Sorcerer companion', 'collection': 'sorcerer'},
        ]}
        self.write()

    def write(self):
        self.path.write_text(json.dumps(self.catalog))

    def run_generator(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), '--root', str(self.root), *args], capture_output=True, text=True)

    def test_generation_and_check_enforce_the_same_ids_and_collections(self):
        result = self.run_generator()
        self.assertEqual(result.returncode, 0, result.stderr)
        target = self.root / 'include/p8p/bbs_catalog.inc'
        data = target.read_text()
        self.assertIn('{"62288", P8P_CART_FEATURE_OFFLINE_ARCADE}', data)
        self.assertIn('{"mot_sorcerergame", P8P_CART_FEATURE_OFFLINE_SORCERER}', data)
        self.assertEqual(self.run_generator('--check').returncode, 0)
        self.catalog['carts'][0]['id'] = '64085'
        self.write()
        self.assertEqual(self.run_generator('--check').returncode, 1)
        self.assertEqual(target.read_text(), data)
        self.assertEqual(self.run_generator().returncode, 0)
        self.assertNotIn('62288', target.read_text())

    def test_bad_ids_duplicates_versions_and_collections_do_not_generate(self):
        for mutation in ('path', 'duplicate', 'version', 'collection', 'empty_group'):
            with self.subTest(mutation=mutation):
                original = json.loads(self.path.read_text())
                if mutation == 'path': self.catalog['carts'][0]['id'] = '../cart'
                if mutation == 'duplicate': self.catalog['carts'][1]['id'] = '62288'
                if mutation == 'version': self.catalog['version'] = 2
                if mutation == 'collection': self.catalog['carts'][1]['collection'] = 'unknown'
                if mutation == 'empty_group': self.catalog['carts'].pop()
                self.write()
                self.assertNotEqual(self.run_generator().returncode, 0)
                self.assertFalse((self.root / 'include/p8p/bbs_catalog.inc').exists())
                self.catalog = original
                self.write()

if __name__ == '__main__':
    unittest.main()
