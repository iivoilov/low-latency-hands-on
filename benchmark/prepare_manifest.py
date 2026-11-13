#!/usr/bin/env python3
"""Fingerprint a checkout before building and running separate-host trials."""
import argparse
import hashlib
import json
from pathlib import Path


def create_manifest(root):
    root = root.resolve()
    paths = [root / 'CMakeLists.txt', root / 'configs/baseline.json',
             root / 'processors/rust/Cargo.toml', root / 'processors/rust/Cargo.lock',
             root / 'processors/rust/build.rs']
    for name in ('simulator/src', 'simulator/include', 'simulator/tests',
                 'processors/cpp/src', 'processors/cpp/include', 'processors/cpp/tests',
                 'processors/rust/src', 'benchmark'):
        paths.extend(p for p in (root / name).rglob('*')
                     if p.suffix in {'.cpp', '.hpp', '.rs', '.py'} and '__pycache__' not in p.parts)
    files = {}
    for path in sorted(set(paths)):
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError('Expected source file inside checkout: ' + str(path))
        files[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    source_id = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {'source_id': source_id, 'files': files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    manifest = create_manifest(args.root)
    destination = args.root / '.deploy/source-manifest.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'source_id': manifest['source_id'], 'files': len(manifest['files'])}))


if __name__ == '__main__':
    main()
