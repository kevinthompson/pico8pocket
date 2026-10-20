#!/usr/bin/env python3
"""Collect complete cached Rust dependency and standard-library license notices."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LICENSE_PREFIXES = ('LICENSE', 'LICENCE', 'COPYING')
NOTICE_PREFIXES = LICENSE_PREFIXES + ('NOTICE',)
SUPPLEMENTAL_DIRECTORY = ROOT / 'tools/plugin-licenses'
SUPPLEMENTAL_LICENSES = {
    (name, '1.41.0'): 'extism-1.41.0.txt'
    for name in ('extism-convert', 'extism-convert-macros', 'extism-manifest')
}
EXTISM_REPOSITORY = 'https://github.com/extism/extism'
EXTISM_REVISION = '051b765bb6abe4fe95d5e4f489f03ad7b137edbc'


def reachable_packages(metadata):
    """Follow normal/build edges from the plugin, omitting dev-only packages."""
    resolve = metadata.get('resolve') or {}
    root = resolve.get('root')
    if not root:
        raise ValueError('cargo metadata has no plugin resolve root')
    packages = {package['id']: package for package in metadata['packages']}
    nodes = {node['id']: node for node in resolve['nodes']}
    pending = [root]
    visited = set()
    while pending:
        identifier = pending.pop()
        if identifier in visited:
            continue
        if identifier not in packages or identifier not in nodes:
            raise ValueError(f'incomplete cargo resolve graph: {identifier}')
        visited.add(identifier)
        for dependency in nodes[identifier]['deps']:
            if any(kind['kind'] != 'dev' for kind in dependency['dep_kinds']):
                pending.append(dependency['pkg'])
    return sorted((packages[identifier] for identifier in visited if identifier != root),
                  key=lambda package: (package['name'], package['version']))


def notice_files(directory):
    """Include named notices and every file in nested license directories."""
    result = []
    for path in directory.rglob('*'):
        if not path.is_file():
            continue
        relative = path.relative_to(directory)
        if (path.name.upper().startswith(NOTICE_PREFIXES)
                or any(part.upper().startswith(LICENSE_PREFIXES) for part in relative.parts[:-1])):
            result.append(path)
    return result


def read_notice(path, label):
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ValueError(f'missing license/notice: {label}') from error
    if not data.strip():
        raise ValueError(f'empty license/notice: {label}')
    return data


def append_notices(output, directory, paths, label):
    for path in sorted(set(paths), key=lambda item: item.relative_to(directory).as_posix()):
        name = path.relative_to(directory).as_posix()
        data = read_notice(path, f'{label}/{name}')
        output.extend(f'\n--- {name} ---\n'.encode())
        output.extend(data)
        output.extend(b'\n')


def supplemental_license(package, directory, supplemental_directory):
    """Accept an omitted Extism license only for reviewed, exact provenance."""
    label = f'{package["name"]} {package["version"]}'
    filename = SUPPLEMENTAL_LICENSES.get((package['name'], package['version']))
    if not filename:
        raise ValueError(f'missing license text: {label}')
    if package.get('repository') != EXTISM_REPOSITORY or package.get('license') != 'BSD-3-Clause':
        raise ValueError(f'supplemental license provenance mismatch: {label}')
    try:
        vcs = json.loads((directory / '.cargo_vcs_info.json').read_text())
    except (OSError, ValueError) as error:
        raise ValueError(f'missing supplemental license source revision: {label}') from error
    if vcs.get('git', {}).get('sha1') != EXTISM_REVISION:
        raise ValueError(f'supplemental license source revision mismatch: {label}')
    path = supplemental_directory / filename
    return filename, read_notice(path, f'{label}/{filename}')


def generate_notices(metadata, sysroot, rust_version,
                     supplemental_directory=SUPPLEMENTAL_DIRECTORY):
    output = bytearray(b'Pocket Sync plugin: third-party redistribution notices\n'
                       b'Target: wasm32-wasip1\n'
                       b'Includes normal and build dependencies from Cargo.lock.\n')
    for package in reachable_packages(metadata):
        label = f'{package["name"]} {package["version"]}'
        directory = Path(package['manifest_path']).parent
        files = notice_files(directory)
        declared_file = package.get('license_file')
        supplemental = None
        if declared_file:
            path = directory / declared_file
            if not path.resolve().is_relative_to(directory.resolve()):
                raise ValueError(f'license_file escapes crate directory: {label}')
            declared_file = path.relative_to(directory).as_posix()
            read_notice(path, f'{label}/{declared_file}')
            files.append(path)
        elif not any(path.name.upper().startswith(LICENSE_PREFIXES)
                     or any(part.upper().startswith(LICENSE_PREFIXES)
                            for part in path.relative_to(directory).parts[:-1])
                     for path in files):
            supplemental = supplemental_license(package, directory, supplemental_directory)
        if not package.get('license') and not declared_file:
            raise ValueError(f'missing declared license: {label}')
        license_name = package.get('license') or f'license_file: {declared_file}'
        output.extend(f'\n=== {label} ===\nDeclared license: {license_name}\n'.encode())
        append_notices(output, directory, files, label)
        if supplemental:
            filename, data = supplemental
            output.extend(f'\n--- supplemental/{filename} ---\n'.encode())
            output.extend(data)
            output.extend(b'\n')

    rustdoc = Path(sysroot) / 'share/doc/rust'
    copyright_file = rustdoc / 'COPYRIGHT-library.html'
    read_notice(copyright_file, 'Rust/COPYRIGHT-library.html')
    output.extend(f'\n=== Rust standard library ({rust_version}) ===\n'.encode())
    append_notices(output, rustdoc, [copyright_file] + notice_files(rustdoc), 'Rust')
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    cargo = os.environ.get('CARGO', 'cargo')
    rustc = os.environ.get('RUSTC', 'rustc')
    try:
        metadata = json.loads(subprocess.check_output(
            [cargo, 'metadata', '--locked', '--format-version', '1',
             '--filter-platform', 'wasm32-wasip1', '--manifest-path',
             str(ROOT / 'tools/pocket-sync/Cargo.toml')], text=True))
        sysroot = subprocess.check_output([rustc, '--print', 'sysroot'], text=True).strip()
        rust_version = subprocess.check_output([rustc, '--version'], text=True).strip()
        data = generate_notices(metadata, sysroot, rust_version)
        args.output.write_bytes(data)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print(f'plugin notices failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
