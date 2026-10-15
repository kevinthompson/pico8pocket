#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="$PROJECT_ROOT/.deps"
SDK_DIR="$DEPS_DIR/openfgpaSDK"
DIABLO_DIR="$DEPS_DIR/diablo"
FAKE08_DIR="$DEPS_DIR/fake-08"
FORK_DIR="$DEPS_DIR/openfpgaCore"
RUNTIME_DIR="$DEPS_DIR/pocket-runtime"
# What the build uses (SDK_ROOT): Diablo's SDK sources, libraries and
# container tooling, the fork's API headers, and the fork's Pocket runtime.
STAGED_SDK_DIR="$DEPS_DIR/openfpgaos-sdk"

SDK_URL="https://github.com/openfpgaOS/openfgpaSDK.git"
SDK_COMMIT="628a12b551ac8137373c477e97466b84d153d2af"
DIABLO_URL="https://github.com/openfpgaOS/Diablo.git"
DIABLO_COMMIT="8cb3198d50618696e07f5b49ad086a369dc57706"
# Only the SDK and the container tooling are needed; the game sources and
# Diablo's runtime (no save-state service) are left out of the sparse checkout.
DIABLO_PATHS=(src/sdk tools)
# The openfpgaOS fork with native Pocket save states and sleep/wake.  Its API
# headers replace Diablo's; the rest of Diablo's SDK is unchanged by the fork.
FORK_URL="https://github.com/zenibako/openfpgaCore.git"
FORK_COMMIT="a58aacd940c1b433c030b97f8207d2c7e03307c8"
FORK_PATHS=(src/firmware/api)
# Device-tested Pocket runtime, published from 92202ab.  a58aacd adds only
# of_savestate_set_id() to the OS; on this runtime it reports not supported
# and the state's own cart hash is the only check.
RUNTIME_URL="https://github.com/zenibako/openfpgaCore/releases/download/pocket-runtime-92202ab"
RUNTIME_FILES=(os20.rbf_r os.bin loader.bin)
RUNTIME_SHA256=(
    "621a3fb6546c5c57440df21661b611a5ea243f0625f7afb247bb9511d7024755"
    "f6f585308667692f543236c90e73da45c25c9e4f497086c6e780826f3723ab82"
    "fc1f8d37eb0fb322006b4428f0b59d2801c9b74978bd3f24103f5f9bbfca3c99"
)
FAKE08_URL="https://github.com/jtothebell/fake-08.git"
FAKE08_COMMIT="814991a2571ad3970e386cef48f3b148aa1c27b9"
Z8LUA_COMMIT="e6928578d46b61fd5ea30cfcf547e855a30a0553"
Z8LUA_PATCHES=(
    "$PROJECT_ROOT/patches/z8lua-rv32-performance.patch"
    "$PROJECT_ROOT/patches/z8lua-env-fallback.patch"
    "$PROJECT_ROOT/patches/z8lua-upstream-backports.patch"
    "$PROJECT_ROOT/patches/z8lua-vm-fastpath.patch"
    "$PROJECT_ROOT/patches/z8lua-number-parse.patch"
    "$PROJECT_ROOT/patches/z8lua-local-compound.patch"
    "$PROJECT_ROOT/patches/z8lua-number-format.patch"
    "$PROJECT_ROOT/patches/z8lua-split-nonstring.patch"
    "$PROJECT_ROOT/patches/z8lua-eq-fastpath.patch"
    "$PROJECT_ROOT/patches/z8lua-index-chain.patch"
    "$PROJECT_ROOT/patches/z8lua-sandbox-index.patch"
    "$PROJECT_ROOT/patches/z8lua-ccall-fastpath.patch"
    "$PROJECT_ROOT/patches/z8lua-servicepoints.patch"
    "$PROJECT_ROOT/patches/z8lua-pc-register.patch"
    "$PROJECT_ROOT/patches/z8lua-inline-cache.patch"
    "$PROJECT_ROOT/patches/z8lua-vm-tuning.patch"
    "$PROJECT_ROOT/patches/z8lua-call-fastpath.patch"
)

fetch_one() {
    local name="$1" url="$2" commit="$3" destination="$4"

    if [[ -d "$destination/.git" ]]; then
        local current
        current="$(git -C "$destination" rev-parse HEAD)"
        if [[ "$current" != "$commit" ]]; then
            echo "$name is at $current, expected $commit" >&2
            echo "Move $destination aside and run make deps again." >&2
            return 1
        fi
        echo "$name: pinned revision already present"
        return 0
    fi

    if [[ -e "$destination" ]]; then
        echo "$destination exists but is not a Git checkout" >&2
        return 1
    fi

    git clone --no-checkout "$url" "$destination"
    git -C "$destination" fetch --depth 1 origin "$commit"
    git -C "$destination" checkout --detach "$commit"
    echo "$name: checked out $commit"
}

# Shallow, blobless, sparse checkout of a pinned commit.  The commit ID pins
# the content; unlike GitHub's generated tarballs it cannot change upstream.
fetch_sparse() {
    local name="$1" url="$2" commit="$3" destination="$4"
    shift 4

    if [[ -d "$destination/.git" ]]; then
        local current
        current="$(git -C "$destination" rev-parse HEAD)"
        if [[ "$current" != "$commit" ]]; then
            echo "$name is at $current, expected $commit" >&2
            echo "Move $destination aside and run make deps again." >&2
            return 1
        fi
        echo "$name: pinned revision already present"
        return 0
    fi

    if [[ -e "$destination" ]]; then
        echo "$destination exists but is not a Git checkout" >&2
        echo "Move $destination aside and run make deps again." >&2
        return 1
    fi

    # Build in a staging directory so an interrupted fetch never leaves a
    # partial checkout at the destination.
    local staging="$destination.partial"
    rm -rf "$staging"
    git init -q "$staging"
    git -C "$staging" remote add origin "$url"
    git -C "$staging" sparse-checkout set --no-cone "$@"
    git -C "$staging" fetch --depth 1 --filter=blob:none origin "$commit"
    git -C "$staging" checkout -q --detach "$commit"
    mv "$staging" "$destination"
    echo "$name: checked out $commit"
}

verify_sha256() {
    local file="$1" expected="$2" label="$3" actual
    [[ -f "$file" ]] || { echo "Missing $label: $file" >&2; return 1; }
    actual="$(shasum -a 256 "$file" | awk '{print $1}')"
    [[ "$actual" == "$expected" ]] || {
        echo "$label checksum mismatch: $actual" >&2
        return 1
    }
}

# Download each runtime file once and check it against its pin.
fetch_runtime() {
    local i file
    mkdir -p "$RUNTIME_DIR"
    for i in "${!RUNTIME_FILES[@]}"; do
        file="${RUNTIME_FILES[$i]}"
        if [[ -f "$RUNTIME_DIR/$file" ]] &&
           verify_sha256 "$RUNTIME_DIR/$file" "${RUNTIME_SHA256[$i]}" "$file" 2>/dev/null; then
            continue
        fi
        curl -fsSL -o "$RUNTIME_DIR/$file.partial" "$RUNTIME_URL/$file"
        verify_sha256 "$RUNTIME_DIR/$file.partial" "${RUNTIME_SHA256[$i]}" \
            "Pocket runtime $file"
        mv "$RUNTIME_DIR/$file.partial" "$RUNTIME_DIR/$file"
        echo "Pocket runtime: downloaded $file"
    done
}

# Assemble the SDK the build uses.  Rebuilt from scratch whenever any pinned
# input changes, so it never mixes files from two pins.
stage_sdk() {
    local stamp="$STAGED_SDK_DIR/.p8p-inputs"
    local inputs="$DIABLO_COMMIT $FORK_COMMIT ${RUNTIME_SHA256[*]}"
    if [[ -f "$stamp" && "$(cat "$stamp")" == "$inputs" ]]; then
        echo "openfpgaOS SDK: staged SDK already up to date"
        return 0
    fi
    local staging="$STAGED_SDK_DIR.partial" file
    rm -rf "$staging"
    mkdir -p "$staging/runtime/pocket"
    cp -R "$DIABLO_DIR/src" "$DIABLO_DIR/tools" "$staging/"
    # Same filter as openfpgaOS's `make sdk`: every header, subdirectories too.
    rsync -a --delete --include='*/' --include='*.h' --exclude='*' \
        "$FORK_DIR/src/firmware/api/" "$staging/src/sdk/include/"
    for file in "${RUNTIME_FILES[@]}"; do
        cp "$RUNTIME_DIR/$file" "$staging/runtime/pocket/"
    done
    printf '%s\n' "$inputs" > "$staging/.p8p-inputs"
    rm -rf "$STAGED_SDK_DIR"
    mv "$staging" "$STAGED_SDK_DIR"
    echo "openfpgaOS SDK: staged $STAGED_SDK_DIR"
}

mkdir -p "$DEPS_DIR"
fetch_one "openfpgaOS SDK" "$SDK_URL" "$SDK_COMMIT" "$SDK_DIR"
fetch_sparse "Diablo SDK" "$DIABLO_URL" "$DIABLO_COMMIT" \
    "$DIABLO_DIR" "${DIABLO_PATHS[@]/#//}"
fetch_sparse "openfpgaOS fork API" "$FORK_URL" "$FORK_COMMIT" \
    "$FORK_DIR" "${FORK_PATHS[@]/#//}"
for header in of_version.h of_savestate.h; do
    [[ -f "$FORK_DIR/src/firmware/api/$header" ]] || {
        echo "openfpgaOS fork API has no $header" >&2
        exit 1
    }
done
grep -q '^#define OF_API_VERSION_MINOR  *9$' \
    "$FORK_DIR/src/firmware/api/of_version.h" || {
    echo "openfpgaOS fork API is not 0.9.x" >&2
    exit 1
}
fetch_runtime
stage_sdk
fetch_one "Fake-08" "$FAKE08_URL" "$FAKE08_COMMIT" "$FAKE08_DIR"

git -C "$FAKE08_DIR" submodule update --init --depth 1 libs/z8lua
actual_z8lua_commit="$(git -C "$FAKE08_DIR/libs/z8lua" rev-parse HEAD)"
if [[ "$actual_z8lua_commit" != "$Z8LUA_COMMIT" ]]; then
    echo "z8lua is at $actual_z8lua_commit, expected $Z8LUA_COMMIT" >&2
    exit 1
fi
echo "z8lua: checked out $actual_z8lua_commit"

# Later patches may touch context used by earlier ones, so individual
# reverse-apply checks are unreliable.  Record the applied patch set and
# rebuild the pristine pinned tree whenever it changes.
Z8LUA_DIR="$FAKE08_DIR/libs/z8lua"
patch_stamp="$(git -C "$Z8LUA_DIR" rev-parse --absolute-git-dir)/p8p-patches"
patch_digest="$(cat "${Z8LUA_PATCHES[@]}" | shasum -a 256 | awk '{print $1}')"
tree_digest() {
    git -C "$Z8LUA_DIR" diff HEAD | shasum -a 256 | awk '{print $1}'
}
if [[ -f "$patch_stamp" &&
      "$(cat "$patch_stamp")" == "$patch_digest $(tree_digest)" ]]; then
    echo "z8lua: patches already applied"
else
    git -C "$Z8LUA_DIR" checkout -q -- .
    git -C "$Z8LUA_DIR" clean -q -fd
    for z8lua_patch in "${Z8LUA_PATCHES[@]}"; do
        if ! git -C "$Z8LUA_DIR" apply "$z8lua_patch"; then
            echo "z8lua: cannot apply $z8lua_patch" >&2
            exit 1
        fi
        echo "z8lua: applied $(basename "$z8lua_patch")"
    done
    printf '%s %s\n' "$patch_digest" "$(tree_digest)" > "$patch_stamp"
fi
