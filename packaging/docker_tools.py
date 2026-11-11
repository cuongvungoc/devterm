"""Resolve and hold the exact base-image ID for an OS/CPU, including legacy Docker."""
from contextlib import contextmanager
import re
import subprocess


@contextmanager
def target_base(ubuntu, arch):
    platform = 'linux/' + arch
    # A mutable ubuntu:XX.XX tag can point at the other architecture after a
    # concurrent pull. Legacy builders may ignore their default --platform.
    # A created container selects the requested platform and pins its image ID.
    identifier = subprocess.check_output(
        ['docker', 'create', '--platform', platform, 'ubuntu:' + ubuntu, '/bin/true'], text=True).strip().splitlines()[-1]
    if not re.fullmatch(r'[0-9a-f]{64}', identifier):
        raise RuntimeError('Unexpected container ID from Docker')
    try:
        subprocess.run(['docker', 'start', '--attach', identifier], check=True)
        status = subprocess.check_output(['docker', 'inspect', '--format', '{{.State.ExitCode}}', identifier], text=True).strip()
        if status != '0':
            raise RuntimeError(f'{platform} cannot execute. Use a native machine or configure QEMU/binfmt first.')
        image = subprocess.check_output(['docker', 'inspect', '--format', '{{.Image}}', identifier], text=True).strip()
        architecture = subprocess.check_output(['docker', 'image', 'inspect', '--format', '{{.Architecture}}', image], text=True).strip()
        if architecture != arch:
            raise RuntimeError(f'Docker selected {architecture}, expected {arch}')
        yield image
    finally:
        subprocess.run(['docker', 'rm', '--force', identifier], check=False, stdout=subprocess.DEVNULL)
