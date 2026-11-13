#!/usr/bin/env python3
"""Build the Rust receiver and record the exact toolchain, sources and command."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cargo', default='cargo')
    parser.add_argument('--rustc', default='rustc')
    parser.add_argument('--build', default='rust-o3')
    parser.add_argument('--vendor-directory', type=Path, help='Optional checksummed vendor directory for offline builds')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.build):
        parser.error('Use a simple build name')
    root = Path(__file__).resolve().parents[1]
    directory = root / 'build' / args.build
    directory.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which(args.rustc)
    cargo = shutil.which(args.cargo)
    if not compiler or not cargo:
        parser.error('Need installed rustc and cargo, or explicit executable paths')
    environment = os.environ.copy()
    environment['RUSTC'] = compiler
    environment['CARGO_TARGET_DIR'] = str(directory / 'cargo')
    command = [cargo, 'build', '--manifest-path', str(root / 'processors/rust/Cargo.toml'),
               '--release', '--locked', '--verbose']
    if args.vendor_directory:
        command += ['--offline', '--config', 'source.crates-io.replace-with="offline-vendor"',
                    '--config', 'source.offline-vendor.directory=' + json.dumps(str(args.vendor_directory.resolve()))]
    result = subprocess.run(command, env=environment, cwd=root, capture_output=True, text=True)
    (directory / 'build.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    shutil.copy2(directory / 'cargo/release/processor_rust', directory / 'processor_rust')
    crate = root / 'processors/rust'
    paths = [crate / 'Cargo.toml', crate / 'Cargo.lock', *sorted(crate.glob('*.rs')), *sorted((crate / 'src').rglob('*.rs'))]
    files = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    info = {'implementation': 'rust-os', 'binary_name': 'processor_rust',
            'compiler': subprocess.check_output([compiler, '-Vv'], text=True),
            'cargo': subprocess.check_output([cargo, '-V'], text=True).strip(),
            'profile': 'release', 'opt_level': 3, 'lto': False, 'codegen_units': 16,
            'rustflags': environment.get('RUSTFLAGS', ''),
            'encoded_rustflags': environment.get('CARGO_ENCODED_RUSTFLAGS', ''),
            'command': command, 'source_files': files,
            'rust_source_sha256': hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()}
    (directory / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')
    print(json.dumps({'build': args.build, 'compiler': info['compiler'].splitlines()[0],
                      'binary': str(directory / 'processor_rust')}, indent=2))


if __name__ == '__main__':
    main()
