#!/bin/sh
set -eu
plugin_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$plugin_dir/../.." && pwd)
: "${CARGO_TARGET_DIR:=$repo_dir/build/pocket-sync-target}"
export CARGO_TARGET_DIR
"${CARGO:-cargo}" build --locked --release --lib --target wasm32-wasip1 --manifest-path "$plugin_dir/Cargo.toml"
cp "$CARGO_TARGET_DIR/wasm32-wasip1/release/pocket_sync.wasm" "$plugin_dir/plugin.wasm"
mkdir -p "$repo_dir/releases/pocket-sync"
cp "$plugin_dir/plugin.wasm" "$repo_dir/releases/pocket-sync/plugin.wasm"
cp "$repo_dir/plugin.json" "$repo_dir/releases/pocket-sync/plugin.json"
printf 'Built %s/plugin.wasm and %s/releases/pocket-sync/\n' "$plugin_dir" "$repo_dir"
