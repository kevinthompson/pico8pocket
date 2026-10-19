#!/usr/bin/env bash
# Build the independent thumbnail tools; never packages local cartridges.
set -euo pipefail
cd "$(dirname "$0")/.."
OUTPUT="${1:-build/thumbnail-tools}"
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd)"
TINYGO="${TINYGO:-tinygo}"
WASMOPT="${WASMOPT:-wasm-opt}"
command -v "$TINYGO" >/dev/null || { echo 'TinyGo 0.43.0 is required' >&2; exit 1; }
command -v "$WASMOPT" >/dev/null || { echo 'Binaryen 133 wasm-opt is required' >&2; exit 1; }
"$TINYGO" version | grep -q 'tinygo version 0.43.0 ' || { echo 'Expected TinyGo 0.43.0' >&2; exit 1; }
"$WASMOPT" --version | grep -q 'version 133' || { echo 'Expected Binaryen 133' >&2; exit 1; }
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/pico8-thumbnail-build.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT
{
 printf 'Thumbnail tooling third-party notices\n\n'
 for notice in tools/thumbnail-licenses/*.txt; do
  printf '\n--- %s ---\n' "$(basename "$notice")"
  cat "$notice"
 done
} > "$SCRATCH/TOOL-NOTICES.txt"
for system in darwin linux windows; do
 for arch in amd64 arm64; do
  BINARY=cart-thumbnails
  if [[ "$system" == windows ]]; then BINARY=cart-thumbnails.exe; fi
  GOOS="$system" GOARCH="$arch" CGO_ENABLED=0 go build -trimpath -o "$SCRATCH/$BINARY" ./cmd/cart-thumbnails
  cp LICENSE "$SCRATCH/LICENSE.txt"
  cp docs/THUMBNAILS.md "$SCRATCH/THUMBNAILS.md"
  tar -czf "$OUTPUT/cart-thumbnails-$system-$arch.tar.gz" -C "$SCRATCH" "$BINARY" LICENSE.txt THUMBNAILS.md TOOL-NOTICES.txt
 done
done
GOOS=wasip1 GOARCH=wasm CGO_ENABLED=0 go build -trimpath -o "$OUTPUT/cart-thumbnails.wasm" ./cmd/cart-thumbnails
"$TINYGO" build -p 2 -trimpath -target wasip1 -buildmode=c-shared -o "$SCRATCH/plugin.wasm" ./cmd/pocket-plugin
"$WASMOPT" -Oz --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext "$SCRATCH/plugin.wasm" -o "$OUTPUT/plugin.wasm"
cp plugin.json "$OUTPUT/plugin.json"
cp docs/THUMBNAILS.md "$OUTPUT/THUMBNAILS.md"
cp LICENSE "$OUTPUT/LICENSE.txt"
cp "$SCRATCH/TOOL-NOTICES.txt" "$OUTPUT/TOOL-NOTICES.txt"
python3 - "$OUTPUT" <<'PY'
import hashlib
from pathlib import Path
import sys
root=Path(sys.argv[1])
names=['plugin.wasm','plugin.json','cart-thumbnails.wasm','THUMBNAILS.md','LICENSE.txt','TOOL-NOTICES.txt']
names += [f'cart-thumbnails-{system}-{arch}.tar.gz' for system in ('darwin','linux','windows') for arch in ('amd64','arm64')]
(root/'SHA256SUMS').write_text(''.join(f'{hashlib.sha256((root/name).read_bytes()).hexdigest()}  {name}\n' for name in sorted(names)))
PY
printf 'Thumbnail tools built in %s\n' "$OUTPUT"
