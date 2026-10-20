#!/usr/bin/env python3
"""Prepare matched-runtime updates and verify complete public Pocket releases."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_FILES = ('os20.rbf_r', 'os.bin', 'loader.bin')
TOOL_ASSETS = {'plugin.wasm', 'plugin.json', 'OFFLINE-COLLECTIONS.md',
               'LICENSE.txt', 'TOOL-NOTICES.txt'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_pins(text):
    """Read the existing authoritative shell pins without executing the script."""
    result = {}
    for key in ('SDK_URL', 'SDK_COMMIT', 'DIABLO_URL', 'DIABLO_COMMIT', 'FORK_URL',
                'FORK_COMMIT', 'RUNTIME_URL', 'FAKE08_URL', 'FAKE08_COMMIT', 'Z8LUA_COMMIT'):
        values = re.findall(r'^' + key + r'="([^"\n]+)"$', text, re.M)
        if len(values) != 1:
            raise ValueError(f'expected one {key} assignment')
        result[key] = values[0]
        if key.endswith('_COMMIT') and not re.fullmatch(r'[0-9a-f]{40}', values[0]):
            raise ValueError(f'{key} must be a full immutable commit')
    files = re.search(r'^RUNTIME_FILES=\(([^\n]+)\)$', text, re.M)
    hashes = re.search(r'^RUNTIME_SHA256=\(\n(.*?)\n\)', text, re.M | re.S)
    if not files or files[1].split() != list(RUNTIME_FILES) or not hashes:
        raise ValueError('expected the matched os20 runtime set')
    values = re.findall(r'^\s*"([0-9a-f]{64})"\s*$', hashes[1], re.M)
    if len(values) != 3:
        raise ValueError('expected three runtime SHA-256 hashes')
    result['runtime_hashes'] = dict(zip(RUNTIME_FILES, values))
    return result


def runtime_changes(root, commit, tag, hashes):
    """Plan an API/runtime update together, preserving SDK and patched VM pins."""
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('candidate must be a full immutable commit')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}', tag):
        raise ValueError('invalid runtime release tag')
    if set(hashes) != set(RUNTIME_FILES) or any(not re.fullmatch(r'[0-9a-f]{64}', h) for h in hashes.values()):
        raise ValueError('need all three matched os20 runtime hashes')
    text = (root / 'scripts/fetch-deps.sh').read_text()
    old = read_pins(text)
    url = 'https://github.com/zenibako/openfpgaCore/releases/download/' + tag
    if (old['FORK_COMMIT'], old['RUNTIME_URL'], old['runtime_hashes']) == (commit, url, hashes):
        return {}
    updated = text.replace(f'FORK_COMMIT="{old["FORK_COMMIT"]}"', f'FORK_COMMIT="{commit}"')
    updated = updated.replace(f'RUNTIME_URL="{old["RUNTIME_URL"]}"', f'RUNTIME_URL="{url}"')
    replacement = 'RUNTIME_SHA256=(\n' + ''.join(f'    "{hashes[name]}"\n' for name in RUNTIME_FILES) + ')'
    updated = re.sub(r'^RUNTIME_SHA256=\(\n.*?\n\)', lambda _: replacement, updated, flags=re.M | re.S)
    changes = {'scripts/fetch-deps.sh': updated}
    old_tag = old['RUNTIME_URL'].rsplit('/', 1)[1]
    for name in ('README.md', 'docs/architecture.md', 'THIRD_PARTY_NOTICES.md'):
        original = (root / name).read_text()
        if old_tag not in original:
            raise ValueError(f'{name} does not document the current runtime; reconcile it first')
        updated = original.replace(old_tag, tag).replace(old['FORK_COMMIT'], commit)
        updated = updated.replace('`' + old['FORK_COMMIT'][:7] + '`', '`' + commit[:7] + '`')
        if updated != original:
            changes[name] = updated
    return changes


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args], text=True).strip()


def prepare(source, runtime, tag, output, apply=False):
    # Validate before interpolating the tag in Git revision syntax.
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}', tag):
        raise ValueError('invalid runtime release tag')
    candidate = git(source, 'rev-parse', '--verify', 'HEAD^{commit}')
    if git(source, 'rev-parse', '--verify', f'refs/tags/{tag}^{{commit}}') != candidate:
        raise ValueError('candidate checkout does not match the runtime release tag')
    if git(source, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('candidate runtime source must be a clean, dedicated checkout')
    api = source / 'src/firmware/api'
    version = (api / 'of_version.h').read_text()
    for name, value in (('MAJOR', 0), ('MINOR', 9)):
        if not re.search(r'^#define\s+OF_API_VERSION_' + name + rf'\s+{value}\s*$', version, re.M):
            raise ValueError('runtime API must match the pinned Diablo SDK (0.9.x)')
    if not (api / 'of_savestate.h').is_file():
        raise ValueError('candidate API lacks save-state support')
    hashes = {}
    for name in RUNTIME_FILES:
        path = runtime / name
        if path.is_symlink() or not path.is_file() or not path.stat().st_size:
            raise ValueError(f'missing or invalid matched runtime file: {name}')
        hashes[name] = digest(path.read_bytes())
    changes = runtime_changes(ROOT, candidate, tag, hashes)
    old = read_pins((ROOT / 'scripts/fetch-deps.sh').read_text())
    diff = git(source, 'diff', '--stat', old['FORK_COMMIT'], candidate, '--')
    patch = ''.join(''.join(difflib.unified_diff(
        (ROOT / name).read_text().splitlines(keepends=True), text.splitlines(keepends=True),
        fromfile='a/' + name, tofile='b/' + name)) for name, text in changes.items())
    report = (f'# Matched os20 runtime candidate\n\nRelease checkout: `{git(ROOT, "rev-parse", "HEAD")}`\n\n'
              f'Previous API/runtime source: `{old["FORK_COMMIT"]}`\n\nCandidate: `{candidate}`\n\n'
              f'Release tag: `{tag}`\n\nUpdate needed: {"yes" if changes else "no"}\n\n'
              f'```text\n{diff or "(no source changes)"}\n```\n\n'
              + ''.join(f'- `{name}`: `{hashes[name]}`\n' for name in RUNTIME_FILES)
              + '\nHashes identify the downloaded candidate; they do not prove device compatibility.\n'
              'Review the release assets and exact source endpoints. SDK/VM pins and os20 are preserved.\n'
              'Apply runtime-pin.patch only after git apply --check, sign a commit, and require\n'
              'complete distribution CI plus hardware checks before publishing. An empty patch is a no-op.\n')
    output.mkdir(parents=True, exist_ok=True)
    (output / 'runtime-pin.patch').write_text(patch)
    (output / 'REVIEW.md').write_text(report)
    if apply:
        for name, text in changes.items():
            (ROOT / name).write_text(text)
    print(report)


def validate_tools(tools):
    present = {p.name for p in tools.iterdir()}
    if present - {'SHA256SUMS'} != TOOL_ASSETS or any(p.is_symlink() or not p.is_file() for p in tools.iterdir()):
        raise ValueError('offline plugin assets must be complete, flat and contain no extras')
    if (tools / 'plugin.wasm').read_bytes()[:4] != b'\x00asm':
        raise ValueError('plugin.wasm is not WASM')
    try:
        manifest = json.loads((tools / 'plugin.json').read_text())
        canonical = json.loads((ROOT / 'plugin.json').read_text())
    except (ValueError, UnicodeError) as error:
        raise ValueError('invalid offline plugin manifest') from error
    if (not isinstance(manifest, dict) or manifest != canonical
            or manifest.get('name') != 'Pico8Pocket Offline Collections'
            or manifest.get('allowed_hosts') != ['www.lexaloffle.com']):
        raise ValueError('offline plugin manifest differs from canonical metadata or permissions')


def core_files(core, pins):
    """Verify runtime identity and exclude private files from the actual public ZIP."""
    hashes = {}
    with zipfile.ZipFile(core) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise ValueError('duplicate core ZIP entries')
        for entry in archive.infolist():
            name = entry.filename
            path = PurePosixPath(name)
            if (path.is_absolute() or '..' in path.parts or '\\' in name or not path.parts
                    or path.parts[0] not in ('Cores', 'Assets', 'Platforms')
                    or (entry.external_attr >> 16) & 0o170000 == 0o120000
                    or name.lower().endswith(('.p8', '.p8.png', '.zip', '.sav'))
                    or ('cards' in path.parts and not entry.is_dir() and path.name != 'README.txt')):
                raise ValueError(f'non-public core ZIP entry: {name}')
            if not entry.is_dir():
                hashes[name] = digest(archive.read(name))
        locations = {'os20.rbf_r': 'Cores/zenibako.pico8/os20.rbf_r',
                     'loader.bin': 'Cores/zenibako.pico8/loader.bin',
                     'os.bin': 'Assets/pico8/common/os.bin'}
        for name, location in locations.items():
            if hashes.get(location) != pins['runtime_hashes'][name]:
                raise ValueError(f'packaged runtime does not match pin: {name}')
        elf = archive.read('Assets/pico8/common/pico8pocket.elf')
        if len(elf) < 52 or elf[:6] != b'\x7fELF\x01\x01' or elf[18:20] != b'\xf3\x00':
            raise ValueError('core program must be little-endian 32-bit RISC-V ELF')
        definition = json.loads(archive.read('Cores/zenibako.pico8/core.json'))['core']
        if definition['cores'][0]['filename'] != 'os20.rbf_r':
            raise ValueError('core must use the matched os20 variant')
    return hashes


def provenance(core, output, compiler):
    if compiler != '13.2.0':
        raise ValueError('CI release build requires GCC 13.2.0')
    if git(ROOT, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('release source must be committed and clean')
    pins = read_pins((ROOT / 'scripts/fetch-deps.sh').read_text())
    record = {'source_commit': git(ROOT, 'rev-parse', 'HEAD'), 'pins': pins,
              'compiler': compiler, 'files': core_files(core, pins),
              'commercial_assets_included': False}
    output.write_text(json.dumps(record, indent=2) + '\n')


def checksums(output):
    names = sorted(p.name for p in output.iterdir() if p.name != 'SHA256SUMS')
    (output / 'SHA256SUMS').write_text(''.join(f'{digest((output / name).read_bytes())}  {name}\n' for name in names))


def assemble(core_dir, tools, output):
    if git(ROOT, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('release source must be committed and clean')
    validate_tools(tools)
    version = json.loads((ROOT / 'dist/core/core.json').read_text())['core']['metadata']['version']
    core_name = f'pico8pocket-v{version}.zip'
    if {p.name for p in core_dir.iterdir()} != {core_name, 'CORE-PROVENANCE.json'}:
        raise ValueError('core artifacts must contain only the public ZIP and provenance')
    record = json.loads((core_dir / 'CORE-PROVENANCE.json').read_text())
    pins = read_pins((ROOT / 'scripts/fetch-deps.sh').read_text())
    if (record['source_commit'] != git(ROOT, 'rev-parse', 'HEAD') or record['pins'] != pins
            or record['compiler'] != '13.2.0' or record['files'] != core_files(core_dir / core_name, pins)):
        raise ValueError('core artifact provenance differs from this release commit')
    output.mkdir(parents=True, exist_ok=False)
    for name in TOOL_ASSETS:
        shutil.copyfile(tools / name, output / name)
    for name in (core_name, 'CORE-PROVENANCE.json'):
        shutil.copyfile(core_dir / name, output / name)
    subprocess.run(['git', '-C', str(ROOT), 'archive', '--format=tar.gz', '-o',
                    str(output.resolve() / 'pico8pocket-source.tar.gz'), 'HEAD'], check=True)
    checksums(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prepare_parser = commands.add_parser('prepare-runtime')
    prepare_parser.add_argument('--source', type=Path, required=True)
    prepare_parser.add_argument('--runtime', type=Path, required=True)
    prepare_parser.add_argument('--tag', required=True)
    prepare_parser.add_argument('--out', type=Path, default=ROOT / 'build/runtime-update')
    prepare_parser.add_argument('--apply', action='store_true')
    prov = commands.add_parser('provenance')
    prov.add_argument('--core', type=Path, required=True)
    prov.add_argument('--out', type=Path, required=True)
    prov.add_argument('--compiler', required=True)
    dist = commands.add_parser('assemble')
    dist.add_argument('--core-dir', type=Path, required=True)
    dist.add_argument('--tools', type=Path, required=True)
    dist.add_argument('--out', type=Path, default=ROOT / 'build/distribution')
    args = parser.parse_args()
    if args.command == 'prepare-runtime':
        prepare(args.source.resolve(), args.runtime.resolve(), args.tag, args.out.resolve(), args.apply)
    elif args.command == 'provenance':
        provenance(args.core.resolve(), args.out.resolve(), args.compiler)
    else:
        assemble(args.core_dir.resolve(), args.tools.resolve(), args.out.resolve())


if __name__ == '__main__':
    main()
