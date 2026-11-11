#!/usr/bin/env python3
"""Install each .deb in a matching Ubuntu/Mint APT environment and test its lifecycle."""
import argparse
import os
from pathlib import Path
import re
import subprocess
from docker_tools import target_base

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packages', nargs='+', type=Path)
    args = parser.parse_args()
    for path in args.packages:
        path = path.resolve()
        match = re.fullmatch(r'devterm_.+~ubuntu(22\.04|24\.04)_(amd64|arm64)\.deb', path.name)
        mint = re.fullmatch(r'devterm_.+~linuxmint22\.1_amd64\.deb', path.name)
        if match:
            ubuntu, arch = match.groups()
            image = f'devterm-verify:{ubuntu}-{arch}'
        elif mint:
            ubuntu, arch = '24.04', 'amd64'
            image = 'devterm-verify:linuxmint22.1-amd64'
        else:
            parser.error('Expected a DevTerm Ubuntu or Mint 22.1 amd64 package name: ' + str(path))
        try:
            with target_base(ubuntu, arch) as base:
                run(['docker', 'build', '--platform', 'linux/' + arch, '--build-arg', 'BASE_IMAGE=' + base,
                     '--build-arg', 'LINUXMINT=' + ('22.1' if mint else ''),
                     '-t', image, '-f', ROOT / 'packaging/Dockerfile.verify', ROOT / 'packaging'])
            run(['docker', 'run', '--rm', '--platform', 'linux/' + arch,
                 '--mount', f'type=bind,source={ROOT},target=/source,readonly',
                 '--mount', f'type=bind,source={path.parent},target=/packages,readonly',
                 '--mount', f'type=bind,source={path.parent},target=/reports',
                 '-e', f'OUTPUT_UID={os.getuid()}', '-e', f'OUTPUT_GID={os.getgid()}',
                 image, 'python3', '/source/packaging/verify-installed.py', '/packages/' + path.name,
                 '--report', '/reports/' + path.stem + '.install.json'])
        finally:
            subprocess.run(['docker', 'image', 'rm', image], check=False)


if __name__ == '__main__':
    main()
