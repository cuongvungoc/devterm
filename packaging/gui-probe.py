#!/usr/bin/env python3
"""Exercise the INSTALLED launcher, PTY and host-child library environment on X11."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


def wait_for(predicate, label, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise RuntimeError('Timeout: ' + label)


def main():
    if not Path('/.dockerenv').exists():
        raise SystemExit('Run via packaging/verify-debs.py in its disposable Docker test environment.')
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix='devterm-package-gui-'))
    env_file = root / 'children.jsonl'
    env_writer = root / 'child.py'
    env_writer.write_text('''import ctypes, json, os
from pathlib import Path
glib = ctypes.CDLL('libglib-2.0.so.0')
record = dict(env={k:os.environ.get(k) for k in ['LD_LIBRARY_PATH','LD_PRELOAD','GTK_PATH','GIO_MODULE_DIR','GIO_EXTRA_MODULES','GSETTINGS_SCHEMA_DIR','GI_TYPELIB_PATH']},
              path=os.environ.get('PATH',''),
              maps=[line for line in Path('/proc/self/maps').read_text().splitlines() if 'libglib-2.0.so' in line],
              glib_minor=ctypes.c_uint.in_dll(glib, 'glib_minor_version').value)
with open(os.environ['DEVTERM_PROBE_FILE'],'a') as f:f.write(json.dumps(record)+'\\n')
''')
    shell = root / 'shell'
    shell.write_text(f'#!/bin/sh\n/usr/bin/python3 "{env_writer}"\nexec /bin/sh\n')
    shell.chmod(0o755)
    client_file = root / 'client.jsonl'
    clients = root / 'clients'
    clients.mkdir()
    ssh = clients / 'ssh'
    ssh.write_text(f'#!/bin/sh\nDEVTERM_PROBE_FILE="{client_file}" /usr/bin/python3 "{env_writer}"\nprintf "PACKAGED_CLIENT_READY\\n"\nsleep 10\n')
    ssh.chmod(0o755)
    config = root / 'config/devterm'
    config.mkdir(parents=True)
    profile = dict(id=str(uuid.uuid4()), name='Packaged helper probe', type=0, host='127.0.0.1',
                   port=22, username='tester', identity='', device='', baud=115200, data_bits=8,
                   stop_bits=1, parity='none', flow='none', timeout=5, password_auth=False,
                   remember=False, auto_login=False, send_initial_enter=False,
                   login_prompt='login:', password_prompt='password:', ready_prompt='router#')
    (config / 'sessions.json').write_text(json.dumps(dict(version=1, sessions=[profile])))
    env = dict(os.environ, SHELL=str(shell), DEVTERM_PROBE_FILE=str(env_file), G_DEBUG='fatal-criticals',
               GDK_BACKEND='x11', GSK_RENDERER='cairo', GTK_A11Y='none', GTK_IM_MODULE='simple',
               GIO_USE_VFS='local', GSETTINGS_BACKEND='memory',
               XDG_CONFIG_HOME=str(config.parent), PATH=str(clients) + ':' + os.environ['PATH'])
    log = (root / 'app.log').open('w')
    app = subprocess.Popen(['/usr/bin/devterm'], env=env, stdout=log, stderr=log)

    def x(*arguments):
        result = subprocess.check_output(['xdotool', *map(str, arguments)], text=True, stderr=subprocess.DEVNULL).strip()
        time.sleep(.15)
        return result

    def window(title='^DevTerm$'):
        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', title], text=True, capture_output=True)
        values = [line for line in result.stdout.splitlines() if line.isdecimal()]
        return values[-1] if values else None

    def children():
        return [json.loads(line) for line in env_file.read_text().splitlines()] if env_file.exists() else []

    try:
        main_window = wait_for(window, 'installed application window')
        wait_for(lambda: len(children()) == 1, 'first PTY child')
        x('windowfocus', '--sync', main_window)
        # QEMU can expose the mapped window before GTK has completed its
        # initial focus transfer. Click the actual terminal before typing.
        x('mousemove', '--window', main_window, 250, 250, 'click', 1)
        time.sleep(1)
        command_file = root / 'command.out'
        x('type', '--clearmodifiers', '--delay', 15, f"printf 'PACKAGED_PTY_OK\\n' > '{command_file}'")
        x('key', '--clearmodifiers', 'Return')
        wait_for(lambda: command_file.exists() and command_file.read_text() == 'PACKAGED_PTY_OK\n', 'interactive shell command')
        x('mousemove', '--window', main_window, 65, 90, 'click', 1)
        wait_for(lambda: len(children()) == 2, 'Split Right PTY')
        x('mousemove', '--window', main_window, 180, 90, 'click', 1)
        wait_for(lambda: len(children()) == 3, 'Split Down PTY')
        x('key', '--clearmodifiers', 'ctrl+shift+t')
        wait_for(lambda: len(children()) == 4, 'new tab PTY')
        geometry = dict(line.split('=', 1) for line in x('getwindowgeometry', '--shell', main_window).splitlines() if '=' in line)
        width = int(geometry['WIDTH'])
        # Draft is an editor, including in the package built with private GTK.
        unexpected = root / 'draft-executed'
        x('mousemove', '--window', main_window, width - 95, 90, 'click', 1)
        x('type', '--clearmodifiers', '--delay', 15, f"printf unexpected > '{unexpected}'")
        x('key', '--clearmodifiers', 'Return')
        if unexpected.exists():
            raise RuntimeError('Draft text was executed in the terminal')
        x('key', '--clearmodifiers', 'ctrl+a', 'BackSpace')
        x('mousemove', '--window', main_window, width - 95, 90, 'click', 1)
        # Opening the real Save dialog catches missing schemas/modules/assets.
        x('mousemove', '--window', main_window, width - 40, 90, 'click', 1)
        x('key', '--clearmodifiers', 'Down', 'Return')
        save = wait_for(lambda: window('^Export Pane Text$'), 'packaged Save dialog')
        x('windowfocus', '--sync', save)
        x('key', '--clearmodifiers', 'Escape')
        wait_for(lambda: not window('^Export Pane Text$'), 'Save dialog cancellation')
        # Connect via the actual manager and installed helper. The fake client
        # records its host-library environment without contacting any machine.
        x('mousemove', '--window', main_window, 200, 34, 'click', 1)
        manager = wait_for(lambda: window('^Sessions$'), 'Sessions manager')
        x('mousemove', '--window', manager, 200, 160, 'click', 1)
        x('mousemove', '--window', manager, 310, 99, 'click', 1)
        def read_client():
            data = client_file.read_text() if client_file.exists() else ''
            return [json.loads(line) for line in data.splitlines()] if data.endswith('\n') else []
        client_records = wait_for(read_client, 'client spawned by the installed connection helper')
        for child in children() + client_records:
            if (any(child['env'].values()) or '/usr/lib/devterm/' in child['path'] or not child['maps'] or
                    any('/usr/lib/devterm/' in entry for entry in child['maps'])):
                raise RuntimeError('Private runtime leaked into host program: ' + json.dumps(child))
        x('mousemove', '--window', main_window, width - 40, 34, 'click', 1)
        if app.wait(timeout=20) != 0:
            raise RuntimeError('Installed application exited with an error')
        log.close()
        log_text = (root / 'app.log').read_text()
        if 'CRITICAL' in log_text or 'ERROR' in log_text:
            raise RuntimeError('GTK/GLib runtime error: ' + log_text)
        args.report.write_text(json.dumps(dict(installed_launcher=True, interactive_pty=True,
            split_right=True, split_down=True, new_tab=True, host_children=children(),
            draft_no_execution=True, export_dialog=True, installed_helper=True, host_clients=client_records,
            runtime_environment_clean=True, graceful_exit=True, display='X11/Xvfb'), indent=2) + '\n')
    except Exception:
        log.flush()
        print((root / 'app.log').read_text(), file=sys.stderr)
        raise
    finally:
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
        log.close()


if __name__ == '__main__':
    main()
