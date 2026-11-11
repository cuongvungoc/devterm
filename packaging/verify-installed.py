#!/usr/bin/env python3
"""Run in a disposable clean Ubuntu container, never on the development host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def capture(args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def main():
    if not Path('/.dockerenv').exists() or os.geteuid() != 0:
        raise SystemExit('Use packaging/verify-debs.py; never run this helper on the development host.')
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    artifact = args.package.resolve()
    version = capture(['dpkg-deb', '-f', artifact, 'Version'])
    arch = capture(['dpkg-deb', '-f', artifact, 'Architecture'])
    if capture(['dpkg', '--print-architecture']) != arch:
        raise RuntimeError('Test container architecture does not match the package')
    config = Path('/home/devterm-test/.config/devterm/commands.json')
    config.parent.mkdir(parents=True)
    sentinel = '{"version":1,"commands":[{"id":"retained","name":"Retained","command":"pwd","send_enter":false}]}\n'
    config.write_text(sentinel)
    run(['chown', '-R', 'devterm-test:devterm-test', config.parents[1]])
    # Ubuntu's minimal Docker image excludes most documentation at unpack time.
    # Include this package's docs so copyright/BOM and checksums are also tested.
    Path('/etc/dpkg/dpkg.cfg.d/zz-devterm-test-docs').write_text('path-include=/usr/share/doc/devterm/*\n')
    run(['apt-get', 'update', '-qq'])
    run(['apt-get', 'install', '-y', str(artifact)])
    main_binary = Path('/usr/lib/devterm/devterm')
    helper = main_binary.with_name('devterm-connect')
    assert capture(['/usr/bin/devterm', '--version']).startswith('DevTerm ')
    assert capture([helper, '--version']).startswith('DevTerm Connect ')
    dependency_maps = {}
    for binary in [main_binary, helper]:
        dependencies = capture(['ldd', binary])
        if 'not found' in dependencies:
            raise RuntimeError('Unresolved dependency: ' + dependencies)
        if binary.stat().st_mode & 0o6000:
            raise RuntimeError('Unexpected setuid/setgid binary')
        dependency_maps[binary.name] = dependencies
    if capture(['dpkg', '-V', 'devterm']):
        raise RuntimeError('Installed package files do not match their checksums')
    temp = Path(tempfile.mkdtemp(prefix='devterm-install-test-'))
    temp.chmod(0o777)
    gui_report = temp / 'gui.json'
    run(['runuser', '-u', 'devterm-test', '--', 'xvfb-run', '-a', 'dbus-run-session', '--',
         'python3', '/source/packaging/gui-probe.py', '--report', gui_report])
    gui = json.loads(gui_report.read_text())
    if config.read_text() != sentinel:
        raise RuntimeError('Running the app changed a retained user configuration')
    # Exercise the actual preinst rejection path, altering only this disposable
    # container's OS marker and restoring it immediately afterwards.
    os_release = Path('/etc/os-release')
    original_release = os_release.read_text()
    try:
        os_release.write_text(re.sub(r'^VERSION_ID=.*$', 'VERSION_ID="0.00"', original_release, flags=re.M))
        rejected = subprocess.run(['dpkg', '--unpack', str(artifact)]).returncode != 0
        if not rejected:
            raise RuntimeError('Package accepted an unsupported OS marker')
    finally:
        os_release.write_text(original_release)
    assert capture(['dpkg-query', '-W', '-f=${Version}', 'devterm']) == version
    # A higher revision of the same payload exercises a genuine dpkg upgrade.
    upgrade = temp / 'upgrade'
    run(['dpkg-deb', '-R', artifact, upgrade])
    control = upgrade / 'DEBIAN/control'
    control.write_text(re.sub(r'^Version:.*$', 'Version: ' + version + '.test1', control.read_text(), flags=re.M))
    upgrade_deb = temp / 'upgrade.deb'
    run(['dpkg-deb', '--root-owner-group', '--build', upgrade, upgrade_deb])
    run(['apt-get', 'install', '-y', str(upgrade_deb)])
    assert capture(['dpkg-query', '-W', '-f=${Version}', 'devterm']) == version + '.test1'
    assert config.read_text() == sentinel
    run(['apt-get', 'remove', '-y', 'devterm'])
    assert not Path('/usr/bin/devterm').exists() and not main_binary.exists() and not helper.exists()
    assert config.read_text() == sentinel
    run(['apt-get', 'install', '-y', str(artifact)])
    assert config.read_text() == sentinel
    report = dict(version=version, architecture=arch,
        sha256=hashlib.sha256(artifact.read_bytes()).hexdigest(), clean_apt_install=True,
        dependency_maps=dependency_maps, gui=gui, wrong_os_marker_rejected=rejected,
        upgrade_preserved_config=True, remove_preserved_config=True, reinstall=True,
        hardware_serial=False, physical_arm=False, wayland=False)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    os.chown(args.report, int(os.environ.get('OUTPUT_UID', '0')), int(os.environ.get('OUTPUT_GID', '0')))
    shutil.rmtree(temp)


if __name__ == '__main__':
    main()
