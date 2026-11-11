#!/usr/bin/env python3
"""Executed as root in the disposable, target-architecture builder container."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from runtime import PREFIX, build_runtime


def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def capture(args, **kwargs):
    return subprocess.check_output(list(map(str, args)), text=True, **kwargs).strip()


def main():
    if not Path('/.dockerenv').exists() or os.geteuid() != 0:
        raise SystemExit('Use packaging/build-debs.py; this helper runs only as root inside its Docker builder.')
    source_root, work, output = Path('/source'), Path('/work'), Path('/output')
    release = dict(re.findall(r'^(\w+)="?([^"\n]+)"?$', Path('/etc/os-release').read_text(), re.M))
    ubuntu = release['VERSION_ID']
    arch = capture(['dpkg', '--print-architecture'])
    if release['ID'] != 'ubuntu' or ubuntu not in ('22.04', '24.04') or arch not in ('amd64', 'arm64'):
        raise SystemExit('Unsupported builder OS/architecture')
    if os.environ.get('TARGET_UBUNTU', ubuntu) != ubuntu or os.environ.get('TARGET_ARCH', arch) != arch:
        raise SystemExit('Builder OS/architecture does not match the requested target')
    version = re.search(r"version:\s*'([^']+)'", (source_root / 'meson.build').read_text())[1]
    revision = os.environ.get('PACKAGE_REVISION', '1')
    if not re.fullmatch(r'[0-9]+', revision):
        raise SystemExit('Invalid package revision')
    package_version = f'{version}-{revision}~ubuntu{ubuntu}'
    stem = f'devterm_{package_version}_{arch}'
    jobs = int(os.environ.get('BUILD_JOBS', '2'))
    private = ubuntu == '22.04'
    copyrights = build_runtime(source_root, work, output, jobs) if private else None
    source = work / 'app'
    source.mkdir()
    # Explicit allowlist: no Git metadata, credentials, local configs or skill caches.
    for item in ['meson.build', 'meson_options.txt', 'src', 'data', 'tests', 'packaging', 'README.md']:
        original = source_root / item
        if original.is_dir():
            shutil.copytree(original, source / item, ignore=shutil.ignore_patterns('__pycache__'))
        else:
            shutil.copy2(original, source / item)
    build = work / 'app-build'
    env = dict(os.environ)
    if private:
        env['PKG_CONFIG_PATH'] = str(PREFIX / 'lib/pkgconfig') + ':' + str(PREFIX / 'share/pkgconfig')
        env['PATH'] = str(PREFIX / 'bin') + ':' + env['PATH']
        env['LDFLAGS'] = '-Wl,-rpath,' + str(PREFIX / 'lib')
    run(['meson', 'setup', build, source, '--prefix=/usr', '--bindir=lib/devterm', '--buildtype=release',
         '-Dbundled_runtime=' + ('true' if private else 'false')], env=env)
    run(['meson', 'compile', '-C', build, '-j', str(jobs)], env=env)
    run(['chown', '-R', 'devterm-test:devterm-test', build])
    tests_env = dict(os.environ, GDK_BACKEND='x11', GSK_RENDERER='cairo', GTK_A11Y='none',
                     GTK_IM_MODULE='simple', GSETTINGS_BACKEND='memory', GIO_USE_VFS='local')
    # Run as an ordinary user, including the write-permission and keyring tests.
    try:
        run(['runuser', '-u', 'devterm-test', '--', 'xvfb-run', '-a', 'meson', 'test', '-C', build,
             '--print-errorlogs', '--num-processes', '2', '--timeout-multiplier', '6' if arch == 'arm64' else '1'], env=tests_env)
    finally:
        log = build / 'meson-logs/testlog.txt'
        if log.exists():
            shutil.copy2(log, output / (stem + '.tests.txt'))
    test_records = [json.loads(line) for line in (build / 'meson-logs/testlog.json').read_text().splitlines()]
    # Meson can report skipped GTK suites as "OK"; reject either explicit skips
    # or TAP skips so a release cannot silently omit GUI/connection coverage.
    logs = (build / 'meson-logs/testlog.txt').read_text()
    if any(record['result'] != 'OK' for record in test_records) or re.search(r'(?im)^.*(?:# SKIP|# skip|SKIP:|Skipped:\s+[1-9])', logs):
        raise RuntimeError('Release tests contain skips; see the exported test log')
    stage = work / 'debian/devterm'
    run(['meson', 'install', '-C', build, '--no-rebuild', '--destdir', stage], env=env)
    control = stage / 'DEBIAN'
    control.mkdir()
    bindir = stage / 'usr/bin'
    bindir.mkdir(parents=True, exist_ok=True)
    launcher = bindir / 'devterm'
    launcher.write_text('#!/bin/sh\nexec /usr/lib/devterm/devterm "$@"\n')
    launcher.chmod(0o755)
    private_libs = []
    if private:
        runtime_dest = stage / str(PREFIX).lstrip('/')
        (runtime_dest / 'lib').mkdir(parents=True)
        for library in (PREFIX / 'lib').glob('*.so*'):
            shutil.copy2(library, runtime_dest / 'lib' / library.name, follow_symlinks=False)
        for item in ['share/glib-2.0/schemas', 'share/gtk-4.0', 'share/locale']:
            if (PREFIX / item).exists():
                shutil.copytree(PREFIX / item, runtime_dest / item, symlinks=True)
        # GIO's plugin ABI is stable. Reuse Ubuntu's settings/proxy/TLS modules
        # instead of freezing copies of desktop services in the private runtime.
        triplet = capture(['dpkg-architecture', '-qDEB_HOST_MULTIARCH'])
        (runtime_dest / 'lib/gio').mkdir()
        (runtime_dest / 'lib/gio/modules').symlink_to('/usr/lib/' + triplet + '/gio/modules')
        private_libs = [p for p in (runtime_dest / 'lib').iterdir() if p.is_file() and not p.is_symlink()]
        for name in ['devterm', 'devterm-connect']:
            run(['patchelf', '--set-rpath', '$ORIGIN/runtime/lib', stage / 'usr/lib/devterm' / name])
    for binary in [stage / 'usr/lib/devterm/devterm', stage / 'usr/lib/devterm/devterm-connect', *private_libs]:
        run(['strip', '--strip-unneeded', binary])
    for library in private_libs:
        library.chmod(0o644)
    doc = stage / 'usr/share/doc/devterm'
    doc.mkdir(parents=True)
    for name, original in [('README.md', source / 'README.md'), ('PACKAGING.md', source / 'packaging/README.md')]:
        (doc / (name + '.gz')).write_bytes(gzip.compress(original.read_bytes(), mtime=0))
    copyright_text = ('DevTerm\nSource: local DevTerm repository\n'
                      'Upstream has not declared a distribution license. Consult the owner before public redistribution.\n')
    (doc / 'copyright').write_text(copyright_text)
    if copyrights:
        shutil.copytree(copyrights, doc / 'runtime-copyright')
        shutil.copy2(source / 'packaging/runtime-sources.json', doc / 'runtime-sources.json')
    run(['desktop-file-validate', stage / 'usr/share/applications/com.example.DevTerm.desktop'])
    # dpkg-shlibdeps computes system dependencies. Private SONAMEs are mapped
    # to this package and excluded, without suppressing unknown system libraries.
    debian = work / 'debian'
    debian.mkdir(exist_ok=True)
    (debian / 'control').write_text('Source: devterm\n\nPackage: devterm\nArchitecture: any\nDescription: DevTerm\n')
    shlibs = work / 'private.shlibs'
    entries = []
    for library in private_libs:
        info = capture(['readelf', '-d', library])
        soname = re.search(r'\(SONAME\).*\[([^]]+)\]', info)
        if soname:
            match = re.fullmatch(r'(.+)\.so\.(.+)', soname[1])
            if not match:
                raise RuntimeError('Unrecognized private SONAME: ' + soname[1])
            entries.append(f'{match[1]} {match[2]} devterm\n')
    shlibs.write_text(''.join(entries))
    binaries = [stage / 'usr/lib/devterm/devterm', stage / 'usr/lib/devterm/devterm-connect']
    arguments = ['dpkg-shlibdeps', '-O', '-xdevterm', '-L' + str(shlibs)]
    if private:
        arguments.append('-l' + str(stage / str(PREFIX / 'lib').lstrip('/')))
    for binary in binaries + private_libs:
        arguments.append('-e' + str(binary))
    dependency_output = capture(arguments, cwd=work)
    depends = re.search(r'^shlibs:Depends=(.*)$', dependency_output, re.M)[1]
    # External clients and icon/mime loaders are runtime facilities, not ELF NEEDED entries.
    depends += ', openssh-client, inetutils-telnet, picocom, adwaita-icon-theme, librsvg2-common, shared-mime-info'
    if private:
        depends += ', dconf-gsettings-backend, libegl1, libgl1'
    size = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file() and not p.is_symlink()) // 1024
    (control / 'control').write_text(
        f'Package: devterm\nVersion: {package_version}\nArchitecture: {arch}\n'
        'Maintainer: DevTerm maintainers <noreply@example.invalid>\nSection: utils\nPriority: optional\n'
        f'Installed-Size: {size}\nDepends: {depends}\nRecommends: dbus-user-session, gnome-keyring\n'
        'Description: Native terminal workspace for Linux\n'
        ' GTK4/VTE terminal with tabs, split panes, Quick Commands, Draft and SSH/Telnet/Serial sessions.\n')
    preinst = control / 'preinst'
    preinst.write_text('#!/bin/sh\nset -eu\n. /etc/os-release\n'
                       f'if [ "$ID" != ubuntu ] || [ "$VERSION_ID" != "{ubuntu}" ]; then\n'
                       f'  echo "This DevTerm package requires Ubuntu {ubuntu}. Choose the package matching your OS." >&2\n'
                       '  exit 1\nfi\n')
    preinst.chmod(0o755)
    (control / 'md5sums').write_text(''.join(
        f'{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(stage)}\n'
        for p in sorted(stage.rglob('*')) if p.is_file() and not p.is_symlink() and control not in p.parents))
    artifact = output / (stem + '.deb')
    temporary_artifact = artifact.with_suffix('.deb.partial')
    run(['dpkg-deb', '--root-owner-group', '-Zxz', '-z6', '--build', stage, temporary_artifact])
    temporary_artifact.replace(artifact)
    manifest = dict(package='devterm', version=package_version, ubuntu=ubuntu, architecture=arch,
                    private_runtime=private, runtime_sources=json.loads((source / 'packaging/runtime-sources.json').read_text()) if private else None,
                    build_packages=capture(['dpkg-query', '-W', '-f=${binary:Package}=${Version}\n']).splitlines(),
                    test_groups=len(test_records), tests_passed=True, skips=False, dependencies=depends,
                    python_packages=capture(['python3', '-m', 'pip', 'freeze']).splitlines(),
                    source_date_epoch=os.environ.get('SOURCE_DATE_EPOCH'),
                    sha256=hashlib.sha256(artifact.read_bytes()).hexdigest())
    (output / (stem + '.build.json')).write_text(json.dumps(manifest, indent=2) + '\n')
    for path in output.rglob('*'):
        os.chown(path, int(os.environ.get('OUTPUT_UID', '0')), int(os.environ.get('OUTPUT_GID', '0')), follow_symlinks=False)
    print('Built ' + str(artifact), flush=True)


if __name__ == '__main__':
    try:
        main()
    finally:
        # Retain ownership of cached sources and failure logs even if a build fails.
        output = Path('/output')
        if output.exists():
            for path in output.rglob('*'):
                os.chown(path, int(os.environ.get('OUTPUT_UID', '0')), int(os.environ.get('OUTPUT_GID', '0')), follow_symlinks=False)
