#!/usr/bin/env bash
# Build the Go/TinyGo offline plugin; never packages downloaded carts.
set -euo pipefail
cd "$(dirname "$0")/.."
OUTPUT="${1:-build/offline-plugin}"
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd)"
TINYGO="${TINYGO:-tinygo}"
WASMOPT="${WASMOPT:-wasm-opt}"
command -v "$TINYGO" >/dev/null || { echo 'TinyGo 0.43.0 is required' >&2; exit 1; }
command -v "$WASMOPT" >/dev/null || { echo 'Binaryen 133 wasm-opt is required' >&2; exit 1; }
"$TINYGO" version | grep -q 'tinygo version 0.43.0 ' || { echo 'Expected TinyGo 0.43.0' >&2; exit 1; }
"$WASMOPT" --version | grep -q 'version 133' || { echo 'Expected Binaryen 133' >&2; exit 1; }
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/pico8-offline-build.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT
{
 printf 'Offline collections plugin third-party notices\n\n'
 for notice in go tinygo compiler-rt extism-go-pdk; do
  printf '\n--- %s ---\n' "$notice"
  cat "tools/plugin-licenses/$notice.txt"
 done
} > "$SCRATCH/TOOL-NOTICES.txt"
"$TINYGO" build -p 2 -trimpath -target wasip1 -buildmode=c-shared -o "$SCRATCH/plugin.wasm" ./cmd/pocket-plugin
"$WASMOPT" -Oz --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext "$SCRATCH/plugin.wasm" -o "$OUTPUT/plugin.wasm"
cp plugin.json "$OUTPUT/plugin.json"
cp docs/OFFLINE-COLLECTIONS.md "$OUTPUT/OFFLINE-COLLECTIONS.md"
cp LICENSE "$OUTPUT/LICENSE.txt"
cp "$SCRATCH/TOOL-NOTICES.txt" "$OUTPUT/TOOL-NOTICES.txt"
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
