"""Scanner host handoffs, using only project-owned text fixtures."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib


class ScannerMulticartTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="p8p-scan-")
        self.root = Path(self.temp.name)
        self.scanner = os.environ.get("P8P_COMPAT_SCAN", str(Path(__file__).resolve().parents[1] / "build/tools/pico8pocket_compat_scan"))
        self.parent = self.root / "parent.p8"
        self.parent.write_bytes(self.cart("function _init() cls(3) end"))
        self.pack = self.root / "bbs.p8pack"
        payload = self.cart("function _init() cls(stat(6)=='hello' and 7 or 8) end")
        self.pack.write_bytes(b"P8PBBS1\0" + struct.pack("<II", 1, 0) +
            struct.pack("<48sIIII", b"child", 80, len(payload), zlib.crc32(payload), 0) + payload)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def cart(lua):
        return ("pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n" + lua + "\n").encode()

    def scan(self, *args, pack=True):
        env = dict(os.environ, P8P_EVERY_FRAME="1")
        env.pop("P8P_BBS_PACK", None)
        if pack:
            env["P8P_BBS_PACK"] = str(self.pack)
        return subprocess.run([self.scanner, str(self.parent), *args], env=env,
                              text=True, capture_output=True, timeout=15)

    def test_bench_services_setup_request(self):
        result = self.scan("--bench", "(function() load('#child','parent','hello') end)()", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("kind=load id=child", result.stdout)
        self.assertIn("ok hash=", result.stdout)

    def test_top_level_request_services_before_frame(self):
        self.parent.write_bytes(self.cart("load('#child','parent','hello') error('continued')"))
        result = self.scan("2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("kind=load id=child", result.stdout)

    def test_diagnostic_handoff_returns_to_original_identity(self):
        result = self.scan("--handoff", "(function() load('#child','parent','hello') end)()", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("kind=load id=child", result.stdout)
        self.assertIn("kind=back", result.stdout)
        self.assertIn("handoff verified", result.stdout)

    def test_missing_target_fails_without_resuming_launcher(self):
        result = self.scan("--bench", "(function() load('#missing','parent') end)()", "2")
        self.assertEqual(result.returncode, 6, result.stdout + result.stderr)
        self.assertIn("missing", result.stderr)
        self.assertNotIn("ok hash=", result.stdout)

    def test_cartdata_survives_real_host_transition(self):
        self.parent.write_bytes(self.cart("function _init() cartdata('shared') dset(0,42) cls(3) end"))
        payload = self.cart("function _init() cartdata('shared') if dget(0)~=42 then error('lost cartdata') end cls(7) end")
        self.pack.write_bytes(b"P8PBBS1\0" + struct.pack("<II", 1, 0) +
            struct.pack("<48sIIII", b"child", 80, len(payload), zlib.crc32(payload), 0) + payload)
        result = self.scan("--handoff", "(function() load('#child','parent') end)()", "2")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_compatible_cart_without_pack_still_runs(self):
        result = self.scan("2", pack=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_handoff_without_pack_explains_collection_install(self):
        result = self.scan("--bench", "(function() load('#child','parent') end)()", "2", pack=False)
        self.assertEqual(result.returncode, 6, result.stdout + result.stderr)
        self.assertIn("Offline pack unavailable", result.stderr)


if __name__ == "__main__":
    unittest.main()
