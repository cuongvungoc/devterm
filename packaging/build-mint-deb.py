#!/usr/bin/env python3
"""Repackage a verified Noble amd64 artifact for Linux Mint 22.1 Xia."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def capture(args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', nargs='?', type=Path,
                        default=ROOT / 'dist/devterm_0.1.0-1~ubuntu24.04_amd64.deb')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    version = capture(['dpkg-deb', '-f', source, 'Version'])
    if (capture(['dpkg-deb', '-f', source, 'Package']) != 'devterm' or
            capture(['dpkg-deb', '-f', source, 'Architecture']) != 'amd64' or
            not version.endswith('~ubuntu24.04')):
        parser.error('Source must be a DevTerm Ubuntu 24.04 amd64 package')
    source_hash = sha256(source)
    source_manifest = json.loads(source.with_suffix('.build.json').read_text())
    if (source_manifest['sha256'] != source_hash or not source_manifest['tests_passed'] or
            source_manifest['skips'] or source_manifest['test_groups'] != 9):
        parser.error('Source artifact must match its successful, unskipped build report')
    mint_version = version.removesuffix('~ubuntu24.04') + '~linuxmint22.1'
    output.mkdir(parents=True, exist_ok=True)
    artifact = output / f'devterm_{mint_version}_amd64.deb'
    with tempfile.TemporaryDirectory(prefix='devterm-mint-package-') as folder:
        stage = Path(folder) / 'devterm'
        subprocess.run(['dpkg-deb', '-R', str(source), str(stage)], check=True)
        original_files = {p.relative_to(stage).as_posix(): sha256(p)
                          for p in (stage / 'usr').rglob('*') if p.is_file() and not p.is_symlink()}
        control = stage / 'DEBIAN/control'
        text = re.sub(r'^Version:.*$', 'Version: ' + mint_version, control.read_text(), flags=re.M)
        doc = stage / 'usr/share/doc/devterm/MINT.md.gz'
        doc.write_bytes(gzip.compress((ROOT / 'packaging/MINT.md').read_bytes(), mtime=0))
        size = sum(p.stat().st_size for p in (stage / 'usr').rglob('*')
                   if p.is_file() and not p.is_symlink()) // 1024
        control.write_text(re.sub(r'^Installed-Size:.*$', 'Installed-Size: ' + str(size), text, flags=re.M))
        preinst = stage / 'DEBIAN/preinst'
        preinst.write_text('''#!/bin/sh
set -eu
. /etc/os-release
if [ "${ID:-}" != linuxmint ] || [ "${VERSION_ID:-}" != "22.1" ] || [ "${UBUNTU_CODENAME:-}" != noble ]; then
  echo "This DevTerm package requires Linux Mint 22.1 Xia (Ubuntu Noble base), amd64." >&2
  exit 1
fi
''')
        preinst.chmod(0o755)
        # Keep the tested payload byte-for-byte; only metadata and one doc change.
        assert all(sha256(stage / name) == digest for name, digest in original_files.items())
        (stage / 'DEBIAN/md5sums').write_text(''.join(
            f'{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(stage)}\n'
            for p in sorted((stage / 'usr').rglob('*')) if p.is_file() and not p.is_symlink()))
        temporary = artifact.with_suffix('.deb.partial')
        subprocess.run(['dpkg-deb', '--root-owner-group', '-Zxz', '-z6', '--build', str(stage), str(temporary)], check=True)
        temporary.replace(artifact)
    manifest = dict(package='devterm', version=mint_version, architecture='amd64',
                    target_os=dict(id='linuxmint', version='22.1', codename='xia', ubuntu_codename='noble'),
                    method='repackage-verified-noble-payload', sha256=sha256(artifact),
                    source_artifact=source.name, source_sha256=source_hash,
                    source_test_groups=source_manifest['test_groups'], source_tests_passed=True,
                    source_tests_skipped=False, unchanged_payload_sha256=original_files,
                    dependencies=capture(['dpkg-deb', '-f', artifact, 'Depends']))
    artifact.with_suffix('.build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    checksums = ''.join(f'{sha256(p)}  {p.name}\n' for p in sorted(output.glob('devterm_*.deb')))
    temporary_checksums = output / ('SHA256SUMS.' + str(os.getpid()))
    temporary_checksums.write_text(checksums)
    temporary_checksums.replace(output / 'SHA256SUMS')
    print('Built ' + str(artifact))


if __name__ == '__main__':
    main()
