#!/usr/bin/env python3
"""Build target-specific Ubuntu packages; containers never mount the Docker socket."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
from docker_tools import target_base

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ubuntu', choices=['22.04', '24.04', 'all'], default='all')
    parser.add_argument('--arch', choices=['amd64', 'arm64', 'all'], default='all')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--revision', default='1')
    parser.add_argument('--keep-images', action='store_true', help='Keep only the builder images created by this invocation')
    args = parser.parse_args()
    if args.jobs < 1 or not args.revision.isdecimal():
        parser.error('jobs must be positive and revision must be numeric')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    epoch = subprocess.check_output(['git', '-C', str(ROOT), 'log', '-1', '--format=%ct'], text=True).strip()
    targets = ['22.04', '24.04'] if args.ubuntu == 'all' else [args.ubuntu]
    arches = ['amd64', 'arm64'] if args.arch == 'all' else [args.arch]
    for target in targets:
        for arch in arches:
            platform = 'linux/' + arch
            image = f'devterm-builder:{target}-{arch}'
            succeeded = False
            try:
                with target_base(target, arch) as base:
                    run(['docker', 'build', '--platform', platform, '--build-arg', 'UBUNTU=' + target,
                         '--build-arg', 'BASE_IMAGE=' + base,
                         '-t', image, '-f', ROOT / 'packaging/Dockerfile', ROOT / 'packaging'])
                cache_mount = []
                if target == '22.04':
                    cache = output / '.runtime-cache' / arch
                    cache.mkdir(parents=True, exist_ok=True)
                    cache_mount = ['--mount', f'type=bind,source={cache},target=/usr/lib/devterm/runtime']
                run(['docker', 'run', '--rm', '--platform', platform,
                     '--mount', f'type=bind,source={ROOT},target=/source,readonly',
                     '--mount', f'type=bind,source={output},target=/output',
                     *cache_mount,
                     '-e', 'SOURCE_DATE_EPOCH=' + epoch, '-e', f'BUILD_JOBS={args.jobs}',
                     '-e', f'PACKAGE_REVISION={args.revision}',
                     '-e', f'TARGET_UBUNTU={target}', '-e', f'TARGET_ARCH={arch}',
                     '-e', f'OUTPUT_UID={os.getuid()}', '-e', f'OUTPUT_GID={os.getgid()}',
                     image, 'python3', '/source/packaging/build.py'])
                succeeded = True
            finally:
                if succeeded and not args.keep_images:
                    subprocess.run(['docker', 'image', 'rm', image], check=False)
    packages = sorted(output.glob('devterm_*.deb'))
    checksums = ''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in packages)
    checksum_file = output / ('SHA256SUMS.' + str(os.getpid()))
    checksum_file.write_text(checksums)
    checksum_file.replace(output / 'SHA256SUMS')
    print(f'Packages and SHA256SUMS: {output}')


if __name__ == '__main__':
    main()
