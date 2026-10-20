#!/usr/bin/env bash
# Build public offline plugin artifacts; never packages downloaded carts.
set -euo pipefail
cd "$(dirname "$0")/.."
OUTPUT="${1:-build/offline-plugin}"
mkdir -p "$OUTPUT"
./tools/pocket-sync/build.sh
cp tools/pocket-sync/plugin.wasm "$OUTPUT/plugin.wasm"
cp plugin.json "$OUTPUT/plugin.json"
cp docs/OFFLINE-COLLECTIONS.md "$OUTPUT/OFFLINE-COLLECTIONS.md"
cp LICENSE "$OUTPUT/LICENSE.txt"
python3 scripts/plugin-notices.py "$OUTPUT/TOOL-NOTICES.txt"
python3 - "$OUTPUT" <<'PY'
import hashlib
from pathlib import Path
import sys
root = Path(sys.argv[1])
names = {'plugin.wasm', 'plugin.json', 'OFFLINE-COLLECTIONS.md', 'LICENSE.txt', 'TOOL-NOTICES.txt'}
if {p.name for p in root.iterdir()} - {'SHA256SUMS'} != names:
    raise SystemExit('Offline plugin output contains unexpected files; use a fresh output directory')
(root/'SHA256SUMS').write_text(''.join(f'{hashlib.sha256((root/name).read_bytes()).hexdigest()}  {name}\n' for name in sorted(names)))
PY
printf 'Offline collections plugin built in %s\n' "$OUTPUT"
