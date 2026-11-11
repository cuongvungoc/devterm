"""Build the Ubuntu-patched Noble GUI stack against Jammy's system ABI."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request

PREFIX = Path('/usr/lib/devterm/runtime')


def build_runtime(source_root, work, output, jobs):
    lock_file = source_root / 'packaging/runtime-sources.json'
    lock = json.loads(lock_file.read_text())
    archives = output / 'sources'
    archives.mkdir(exist_ok=True)
    env = dict(os.environ)
    env['PATH'] = f'{PREFIX}/bin:' + env['PATH']
    env['PKG_CONFIG_PATH'] = str(PREFIX / 'lib/pkgconfig') + ':' + str(PREFIX / 'share/pkgconfig')
    # Build tools only. Installed binaries use ELF RUNPATH, never this environment.
    env['LD_LIBRARY_PATH'] = str(PREFIX / 'lib')
    options = {
        'glib2.0': ['-Dtests=false', '-Dintrospection=disabled', '-Ddocumentation=false', '-Dman-pages=disabled'],
        'wayland': ['-Ddocumentation=false', '-Dtests=false'],
        'wayland-protocols': ['-Dtests=false'],
        'gtk4': ['-Dintrospection=disabled', '-Ddocumentation=false', '-Dman-pages=false',
                 '-Dbuild-demos=false', '-Dbuild-examples=false', '-Dbuild-tests=false',
                 '-Dbuild-testsuite=false', '-Dvulkan=disabled', '-Dmedia-gstreamer=disabled',
                 '-Dprint-cups=disabled', '-Dcloudproviders=disabled', '-Dtracker=disabled',
                 '-Dsysprof=disabled'],
        'vte2.91': ['-Dgtk3=false', '-Dgtk4=true', '-Dgir=false', '-Dvapi=false',
                    '-Dglade=false', '-Ddocs=false', '-D_systemd=false'],
    }
    copyrights = PREFIX / 'share/doc/devterm-runtime-copyright'
    copyrights.mkdir(parents=True, exist_ok=True)
    stamps = PREFIX / '.devterm-built'
    stamps.mkdir(exist_ok=True)
    toolchain = subprocess.check_output(['dpkg-query', '-W', '-f=${binary:Package}=${Version}\n'])
    for component in lock['components']:
        name = component['name']
        print(f'Building private runtime: {name} {component["version"]}', flush=True)
        local_archives = work / ('archives-' + name)
        local_archives.mkdir()
        for item in component['files']:
            cached = archives / item['filename']
            algorithm, expected = item['hash'].split(':', 1)
            if not cached.exists() or hashlib.new(algorithm, cached.read_bytes()).hexdigest() != expected:
                temporary = cached.with_suffix(cached.suffix + '.download')
                with urllib.request.urlopen(item['url'], timeout=180) as response, temporary.open('wb') as dest:
                    shutil.copyfileobj(response, dest)
                if hashlib.new(algorithm, temporary.read_bytes()).hexdigest() != expected:
                    temporary.unlink()
                    raise RuntimeError('Source checksum mismatch: ' + item['filename'])
                temporary.replace(cached)
            shutil.copy2(cached, local_archives / cached.name)
        dsc = next(local_archives.glob('*.dsc'))
        fingerprint = hashlib.sha256(lock_file.read_bytes() + toolchain + repr(options[name]).encode()).hexdigest()
        stamp = stamps / name
        if stamp.exists() and stamp.read_text() == fingerprint and (copyrights / (name + '.copyright')).exists():
            print('Reusing verified runtime component: ' + name, flush=True)
            shutil.rmtree(local_archives)
            continue
        source = work / ('source-' + name)
        # Hashes are pinned from Ubuntu's signed source index. dpkg-source also
        # verifies the checksums inside the .dsc and applies Ubuntu's patches.
        subprocess.run(['dpkg-source', '--no-check', '-x', str(dsc), str(source)], check=True)
        shutil.copy2(source / 'debian/copyright', copyrights / (name + '.copyright'))
        build = work / ('build-' + name)
        subprocess.run(['meson', 'setup', str(build), str(source), '--prefix=' + str(PREFIX),
                        '--libdir=lib', '--buildtype=release', '--wrap-mode=nodownload',
                        *options[name]], check=True, env=env)
        subprocess.run(['meson', 'compile', '-C', str(build), '-j', str(jobs)], check=True, env=env)
        subprocess.run(['meson', 'install', '-C', str(build), '--no-rebuild'], check=True, env=env)
        shutil.rmtree(build)
        shutil.rmtree(source)
        shutil.rmtree(local_archives)
        stamp.write_text(fingerprint)
    # Each library resolves its own private dependencies; no loader variables
    # propagate into local shells, ssh, telnet, picocom or their descendants.
    for library in (PREFIX / 'lib').glob('*.so*'):
        if library.is_file() and not library.is_symlink():
            subprocess.run(['patchelf', '--set-rpath', '$ORIGIN', str(library)], check=True)
    for tool in (PREFIX / 'bin').iterdir():
        if tool.is_file() and not tool.is_symlink() and tool.read_bytes()[:4] == b'\x7fELF':
            subprocess.run(['patchelf', '--set-rpath', '$ORIGIN/../lib', str(tool)], check=True)
    return copyrights
