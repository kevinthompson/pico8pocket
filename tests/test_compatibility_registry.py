"""Check that the incompatibility registry stays synchronized with its outputs."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/incompatible-carts.py"
SPEC = importlib.util.spec_from_file_location("incompatible_carts", SCRIPT)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class CompatibilityRegistryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "compatibility").mkdir()
        (self.root / "include/p8p").mkdir(parents=True)
        (self.root / "scripts").mkdir()
        shutil.copyfile(SCRIPT, self.root / "scripts/incompatible-carts.py")
        self.registry = self.root / "compatibility/incompatible-carts.json"
        self.readme = self.root / "README.md"
        self.compiled = self.root / "include/p8p/incompatible_carts.inc"
        self.entry = {
            "title": "Test Arcade",
            "filename": "arcade.p8.png",
            "content_hash": "0123456789abcdef0123456789abcdef",
            "reason": "Requires online cartridges.",
            "workaround": "Open the individual cartridges.",
            "evidence": "Verified launch attempt.",
            "issue": "https://example.test/issues/1",
            "requires": [],
        }
        self.write_entries([self.entry])
        self.readme.write_text(
            "# Test project\n\n"
            + GENERATOR.BEGIN
            + "\nOld table\n"
            + GENERATOR.END
            + "\n\nExisting installation instructions.\n"
        )

    def write_entries(self, entries):
        self.registry.write_text(json.dumps(entries))

    def output_snapshot(self):
        return {
            path: (path.read_bytes(), path.stat().st_mtime_ns)
            if path.exists()
            else None
            for path in (self.readme, self.compiled)
        }

    def test_generation_creates_c_and_replaces_only_readme_marker_contents(self):
        self.assertEqual(GENERATOR.update(self.root, check=False), 0)

        compiled = self.compiled.read_text()
        readme = self.readme.read_text()
        self.assertIn('"Test Arcade"', compiled)
        self.assertIn("0x01, 0x23, 0x45, 0x67", compiled)
        self.assertIn(self.entry["reason"], compiled)
        self.assertIn(self.entry["title"], readme)
        self.assertIn(self.entry["filename"], readme)
        self.assertIn(self.entry["issue"], readme)
        self.assertNotIn("Old table", readme)
        self.assertTrue(readme.startswith("# Test project\n\n" + GENERATOR.BEGIN))
        self.assertTrue(readme.endswith(
            GENERATOR.END + "\n\nExisting installation instructions.\n"
        ))

    def test_conditional_entries_generate_the_collection_capability(self):
        self.entry['requires'] = ['arcade']
        self.write_entries([self.entry])
        self.assertEqual(GENERATOR.update(self.root, check=False), 0)
        self.assertIn('P8P_CART_FEATURE_OFFLINE_ARCADE', self.compiled.read_text())
        self.assertIn('Offline collection required', self.readme.read_text())
        self.entry['requires'] = ['unknown']
        self.write_entries([self.entry])
        with self.assertRaises(ValueError):
            GENERATOR.update(self.root, check=False)

    def test_check_cli_reports_stale_outputs_without_creating_or_changing_them(self):
        before = self.output_snapshot()
        result = subprocess.run(
            [sys.executable, str(self.root / "scripts/incompatible-carts.py"), "--check"],
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("README.md", result.stderr)
        self.assertIn("include/p8p/incompatible_carts.inc", result.stderr)
        self.assertEqual(self.output_snapshot(), before)

    def test_up_to_date_check_and_regeneration_leave_output_files_untouched(self):
        GENERATOR.update(self.root, check=False)
        before = self.output_snapshot()

        self.assertEqual(GENERATOR.update(self.root, check=True), 0)
        self.assertEqual(GENERATOR.update(self.root, check=False), 0)
        self.assertEqual(self.output_snapshot(), before)

    def test_registry_change_requires_and_regenerates_both_outputs(self):
        GENERATOR.update(self.root, check=False)
        before = self.output_snapshot()
        updated = dict(self.entry, title="Updated Arcade", reason="Needs a companion cart.")
        self.write_entries([updated])

        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(GENERATOR.update(self.root, check=True), 1)
        self.assertIn("README.md", errors.getvalue())
        self.assertIn("include/p8p/incompatible_carts.inc", errors.getvalue())
        self.assertEqual(self.output_snapshot(), before)

        self.assertEqual(GENERATOR.update(self.root, check=False), 0)
        for path in (self.readme, self.compiled):
            self.assertNotEqual(path.read_bytes(), before[path][0])
            self.assertIn(updated["title"], path.read_text())
            self.assertIn(updated["reason"], path.read_text())
            self.assertNotIn(self.entry["title"], path.read_text())
        self.assertEqual(GENERATOR.update(self.root, check=True), 0)

    def test_duplicate_content_hashes_are_rejected_before_output_changes(self):
        self.write_entries([self.entry, dict(self.entry, title="Renamed Arcade")])
        before = self.output_snapshot()

        with self.assertRaises(ValueError):
            GENERATOR.update(self.root, check=False)
        self.assertEqual(self.output_snapshot(), before)

    def test_malformed_content_hashes_are_rejected(self):
        for fingerprint in (
            "0" * 31,
            "0" * 33,
            "A" * 32,
            "g" * 32,
            " " + "0" * 31,
        ):
            with self.subTest(fingerprint=fingerprint):
                self.write_entries([dict(self.entry, content_hash=fingerprint)])
                with self.assertRaises(ValueError):
                    GENERATOR.load_entries(self.registry)

    def test_screen_explanation_accepts_255_characters_and_rejects_256(self):
        entry = dict(self.entry, title="Cart", workaround="Use another cart.")
        overhead = len("{title} incompatible.\n\n{workaround}".format(**entry))
        entry["reason"] = "x" * (255 - overhead)
        self.write_entries([entry])
        self.assertEqual(GENERATOR.load_entries(self.registry), [entry])

        entry["reason"] += "x"
        self.write_entries([entry])
        before = self.output_snapshot()
        with self.assertRaises(ValueError):
            GENERATOR.update(self.root, check=False)
        self.assertEqual(self.output_snapshot(), before)

    def test_missing_duplicate_and_reversed_markers_are_rejected_without_writes(self):
        begin, end = GENERATOR.BEGIN, GENERATOR.END
        for contents in (
            "No markers",
            begin,
            end,
            begin + begin + end,
            begin + end + end,
            end + begin,
        ):
            with self.subTest(contents=contents):
                self.readme.write_text(contents)
                before = self.output_snapshot()
                with self.assertRaises(ValueError):
                    GENERATOR.update(self.root, check=False)
                self.assertEqual(self.output_snapshot(), before)


if __name__ == "__main__":
    unittest.main()
