"""Check release metadata using the real SD-tree assembler and synthetic inputs."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PackageMetadataTests(unittest.TestCase):
    """Verify declarations survive assembly without requiring a private cart or compiler."""

    def test_ai_declaration_is_in_assembled_tree(self):
        """Keep the public AI declaration in the exact location read by catalog updaters."""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for relative in ('dist/core', 'dist/platforms', 'dist/instance'):
                shutil.copytree(ROOT / relative, root / relative)
            script = root / 'scripts/assemble-pocket.sh'
            script.parent.mkdir()
            shutil.copyfile(ROOT / 'scripts/assemble-pocket.sh', script)
            files = {
                '.obj/pico8pocket/app.elf': b'synthetic program fixture',
                '.deps/openfpgaos-sdk/runtime/pocket/os20.rbf_r': b'bitstream fixture',
                '.deps/openfpgaos-sdk/runtime/pocket/loader.bin': b'loader fixture',
                '.deps/openfpgaos-sdk/runtime/pocket/os.bin': b'runtime fixture',
                'src/app/pico8pocket.ini': b'config fixture',
                'assets/p8p_slot_pad.bin': b'padding fixture',
                'assets/cards/README.txt': b'Provide your own carts.',
                'LICENSE': b'license fixture',
                'THIRD_PARTY_NOTICES.md': b'notice fixture',
                '.deps/openfgpaSDK/LICENSE': b'SDK license fixture',
                '.deps/openfgpaSDK/NOTICE': b'SDK notice fixture',
                '.deps/fake-08/LICENSE.MD': b'Fake-08 license fixture',
            }
            for name, data in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            env = dict(os.environ, SDK_ROOT=str(root / '.deps/openfpgaos-sdk'), BUNDLE_LOCAL_CARTS='0')
            subprocess.run(['bash', str(script)], env=env, check=True, capture_output=True)
            path = root / 'build/pocket/pico8pocket/Cores/zenibako.pico8/updaters.json'
            self.assertEqual(json.loads(path.read_text())['declare_ai'], ['OpenAI Codex', 'Claude Code'])


if __name__ == '__main__':
    unittest.main()
