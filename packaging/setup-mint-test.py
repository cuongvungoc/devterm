#!/usr/bin/env python3
"""Prepare a minimal Xia APT environment inside the disposable verifier only."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import urllib.request

KEYRING_URL = ('https://mirrors.kernel.org/linuxmint-packages/pool/main/l/'
               'linuxmint-keyring/linuxmint-keyring_2022.06.21_all.deb')
KEYRING_SHA256 = 'b71be690c543112ea7b65f43e9bbce9a3f17dd5cc784074858f0824154942a99'


def main():
    if not Path('/.dockerenv').exists() or os.geteuid() != 0:
        raise SystemExit('This helper runs only in the disposable Docker verifier.')
    subprocess.run(['apt-get', 'update'], check=True)
    with tempfile.TemporaryDirectory(prefix='devterm-mint-bootstrap-') as folder:
        Path(folder).chmod(0o755)
        keyring = Path(folder) / 'linuxmint-keyring.deb'
        with urllib.request.urlopen(KEYRING_URL, timeout=60) as response:
            keyring.write_bytes(response.read())
        if hashlib.sha256(keyring.read_bytes()).hexdigest() != KEYRING_SHA256:
            raise RuntimeError('Linux Mint bootstrap keyring checksum mismatch')
        subprocess.run(['apt-get', 'install', '-y', '--no-install-recommends', str(keyring)], check=True)
    Path('/etc/apt/sources.list.d/devterm-mint-test.list').write_text(
        'deb [signed-by=/etc/apt/trusted.gpg.d/linuxmint-keyring.gpg] '
        'https://mirrors.kernel.org/linuxmint-packages xia main upstream import backport\n')
    # Match the upstream priority in the Mint maintainer's Xia Docker setup:
    # https://github.com/clefebvre/docker-images/tree/master/mint22.1/etc/apt
    Path('/etc/apt/preferences.d/devterm-mint-test.pref').write_text(
        'Package: *\nPin: release o=linuxmint,c=upstream\nPin-Priority: 700\n')
    subprocess.run(['apt-get', 'update'], check=True)
    # Install Mint's actual signed identity package; never forge os-release.
    subprocess.run(['apt-get', 'install', '-y', '--allow-downgrades',
                    '-o', 'Dpkg::Options::=--force-confnew', 'base-files/xia'], check=True)
    release = {}
    for line in Path('/etc/os-release').read_text().splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            release[key] = value.strip('"')
    expected = dict(ID='linuxmint', VERSION_ID='22.1', UBUNTU_CODENAME='noble')
    if any(release.get(key) != value for key, value in expected.items()):
        raise RuntimeError('Mint identity package did not produce the expected Xia platform: ' + repr(release))
    report = dict(kind='minimal-mint-apt-environment', os_release=release,
                  base_files=subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', 'base-files'], text=True),
                  keyring_sha256=KEYRING_SHA256, repository='https://mirrors.kernel.org/linuxmint-packages')
    Path('/usr/local/share/devterm-test-platform.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
