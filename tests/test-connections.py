"""Real client/PTY integration tests. All servers/devices are local fixtures."""
import asyncio
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest
import uuid

HELPER = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(Path('build/devterm-connect').resolve())
PASSWORD = 'dummy-test-password-Đường'

def profile(kind=1):
    return dict(id=str(uuid.uuid4()), name='Test connection', host='127.0.0.1', username='tester', identity='', device='',
                login_prompt=r'(?i)(login|username)\s*:\s*$', password_prompt=r'(?i)password\s*:\s*$', ready_prompt=r'router#\s*$',
                parity='none', flow='none', type=kind, port=23, baud=115200, data_bits=8, stop_bits=1, timeout=5,
                password_auth=True, remember=False, auto_login=kind != 0, send_initial_enter=False)

class Connection:
    def __init__(self, settings, password=PASSWORD, env=None):
        config = json.dumps(settings).encode(); secret = password.encode() if password else b''
        fd = os.memfd_create('devterm-test', os.MFD_CLOEXEC)
        os.write(fd, struct.pack('>II', len(config), len(secret)) + config + secret); os.lseek(fd, 0, 0)
        self.master, slave = pty.openpty()
        def setup():
            os.dup2(fd, 3)
            if fd != 3: os.close(fd)
            os.setsid(); fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        self.process = subprocess.Popen([HELPER, '--session'], stdin=slave, stdout=slave, stderr=slave,
                                        pass_fds=tuple(set((fd, 3))), preexec_fn=setup, env=env)
        os.close(slave); os.close(fd); self.output = bytearray()
    def read(self, timeout=.1):
        if select.select([self.master], [], [], timeout)[0]:
            try: self.output.extend(os.read(self.master, 65536))
            except OSError as exc:
                if exc.errno != errno.EIO: raise
        return bytes(self.output)
    def until(self, needle, timeout=8):
        end = time.monotonic() + timeout
        while needle not in self.output and time.monotonic() < end:
            self.read()
            if self.process.poll() is not None:
                self.read(.01); break
        if needle not in self.output: raise AssertionError(f'Missing {needle!r}: {bytes(self.output)!r}')
        return bytes(self.output)
    def send(self, text): os.write(self.master, text.encode())
    def finish(self, timeout=8):
        end = time.monotonic() + timeout
        while self.process.poll() is None and time.monotonic() < end: self.read()
        code = self.process.wait(timeout=1); self.read(.01); return code
    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try: self.finish(3)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        os.close(self.master)

class Device:
    def __init__(self, serial=False, reject=False, unknown=False, wait_enter=False):
        self.serial, self.reject, self.unknown = serial, reject, unknown
        self.wait_enter, self.initial_enter = wait_enter, False
        self.received, self.error = [], None
        if serial:
            self.master, self.slave = pty.openpty(); self.path = os.ttyname(self.slave)
        else:
            self.server = socket.socket(); self.server.bind(('127.0.0.1', 0)); self.server.listen(1)
            self.server.settimeout(10); self.port = self.server.getsockname()[1]
        self.thread = threading.Thread(target=self.run, daemon=True); self.thread.start()
    def send(self, data):
        if self.serial: os.write(self.master, data)
        else: self.client.sendall(data)
    def line(self, allow_empty=False):
        buf, state = bytearray(), 0
        end = time.monotonic() + 10
        while time.monotonic() < end:
            if self.serial:
                if not select.select([self.master], [], [], .1)[0]: continue
                chunk = os.read(self.master, 1)
            else: chunk = self.client.recv(1)
            if not chunk: return None
            c = chunk[0]
            if not self.serial:
                if state == 1:
                    state = 2 if c in (251,252,253,254) else 3 if c == 250 else 0; continue
                if state == 2: state = 0; continue
                if state == 3:
                    if c == 255: state = 4
                    continue
                if state == 4: state = 0 if c == 240 else 3; continue
                if c == 255: state = 1; continue
            if c in (10,13):
                if buf or allow_empty: return buf.decode()
            elif c != 0: buf.append(c)
        raise TimeoutError('device input timed out')
    def run(self):
        try:
            if self.serial: time.sleep(.5)
            else:
                self.client, _ = self.server.accept(); self.client.settimeout(10)
                self.send(b'\xff\xfb\x01')  # IAC WILL ECHO; deliberately echo password later.
            if self.wait_enter:
                self.initial_enter = self.line(allow_empty=True) == ''
                if not self.initial_enter: raise TimeoutError('initial Enter was not sent')
            if self.unknown:
                self.send(b'Unexpected prompt > '); value = self.line()
                if value is not None: self.received.append(value)
                return
            self.send(b'\x1b[32mUser'); time.sleep(.03); self.send(b'name: \x1b[0m')
            self.received.append(self.line()); self.send(b'\r\nPass'); time.sleep(.03); self.send(b'word: ')
            self.received.append(self.line())
            if self.reject:
                self.send(b'\r\nLogin incorrect\r\nUsername: ')
                # A real login service waits for another attempt. The client
                # must disconnect instead of submitting credentials again.
                extra = self.line()
                if extra is not None: self.received.append(extra)
                return
            self.send(b'\r\n' + self.received[-1].encode() + b'\r\nrouter# ')
            while True:
                command = self.line()
                if command is None or command == 'exit': return
                self.send(('RESULT:' + command + '\r\nrouter# ').encode())
        except Exception as exc: self.error = exc
        finally:
            if not self.serial and hasattr(self, 'client'): self.client.close()
    def close(self):
        if not self.serial:
            if hasattr(self, 'client'):
                try: self.client.shutdown(socket.SHUT_RDWR)
                except OSError: pass
            self.server.close()
        else: os.close(self.master); os.close(self.slave)
        self.thread.join(timeout=1)

class Connections(unittest.TestCase):
    def device_connection(self, **options):
        device = Device(**options); self.addCleanup(device.close)
        settings = profile(2 if device.serial else 1)
        if device.serial: settings['device'] = device.path
        else: settings['port'] = device.port
        settings['send_initial_enter'] = device.wait_enter
        conn = Connection(settings); self.addCleanup(conn.close); return device, conn
    @unittest.skipUnless(shutil.which('telnet'), 'telnet is not installed')
    def test_telnet_login_hidden_echo_and_interaction(self):
        device, conn = self.device_connection(wait_enter=True); conn.until(b'Automatic login complete')
        self.assertTrue(device.initial_enter)
        self.assertEqual(device.received, ['tester', PASSWORD]); self.assertNotIn(PASSWORD.encode(), conn.output)
        conn.send('show\r'); conn.until(b'RESULT:show'); conn.send('exit\r'); self.assertEqual(conn.finish(), 0)
    @unittest.skipUnless(shutil.which('picocom'), 'picocom is not installed')
    def test_serial_login_hidden_echo_and_interaction(self):
        device, conn = self.device_connection(serial=True, wait_enter=True); conn.until(b'Automatic login complete')
        self.assertTrue(device.initial_enter)
        self.assertEqual(device.received, ['tester', PASSWORD]); self.assertNotIn(PASSWORD.encode(), conn.output)
        conn.send('show\r'); conn.until(b'RESULT:show')
        conn.process.send_signal(signal.SIGHUP); conn.finish()
    @unittest.skipUnless(shutil.which('picocom'), 'picocom is not installed')
    def test_serial_open_error_preserves_diagnostic_without_password(self):
        settings = profile(2); settings['device'] = '/dev/devterm-nonexistent-fixture'
        conn = Connection(settings); self.addCleanup(conn.close)
        self.assertNotEqual(conn.finish(), 0); self.assertIn(settings['device'].encode(), conn.output)
        self.assertNotIn(PASSWORD.encode(), conn.output)
    @unittest.skipUnless(shutil.which('telnet'), 'telnet is not installed')
    def test_rejected_login_stops_without_retry(self):
        device, conn = self.device_connection(reject=True); conn.until(b'Login was rejected')
        self.assertNotEqual(conn.finish(), 0); self.assertEqual(device.received, ['tester', PASSWORD]); self.assertNotIn(PASSWORD.encode(), conn.output)
    @unittest.skipUnless(shutil.which('telnet'), 'telnet is not installed')
    def test_unknown_prompt_times_out_without_sending_credentials(self):
        device, conn = self.device_connection(unknown=True); conn.until(b'timed out', 8)
        self.assertNotEqual(conn.finish(), 0); self.assertFalse(device.received); self.assertNotIn(PASSWORD.encode(), conn.output)
    @unittest.skipUnless(shutil.which('telnet'), 'telnet is not installed')
    def test_cancel_login(self):
        device, conn = self.device_connection(unknown=True); conn.until(b'Automatic login in progress')
        conn.send('\x1b'); conn.until(b'cancelled'); self.assertNotEqual(conn.finish(), 0); self.assertFalse(device.received)

try:
    import asyncssh
except ImportError:
    asyncssh = None

@unittest.skipUnless(asyncssh and shutil.which('ssh'), 'SSH tests require OpenSSH and Python asyncssh')
class SSHConnections(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory(prefix='devterm-ssh-test-'); self.addCleanup(self.root.cleanup)
        self.path = Path(self.root.name); self.auth = []; self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True); self.thread.start()
        self.key = asyncssh.generate_private_key('ssh-ed25519')
        auth = self.auth; self.auth_gate = threading.Event(); self.auth_gate.set(); auth_gate = self.auth_gate
        class Server(asyncssh.SSHServer):
            async def begin_auth(self, username):
                while not auth_gate.is_set(): await asyncio.sleep(.01)
                return True
            def password_auth_supported(self): return True
            def validate_password(self, username, password): auth.append((username, password)); return username == 'tester' and password == PASSWORD
        async def shell(process):
            process.stdout.write('SSH_READY\r\n')
            while True:
                try: line = await process.stdin.readline()
                except asyncssh.TerminalSizeChanged: continue
                if not line: break
                line = line.strip()
                if line == 'exit': process.exit(0); return
                if line == 'size':
                    width, height, _, _ = process.get_terminal_size(); process.stdout.write(f'SIZE:{width}x{height}\r\n')
                else: process.stdout.write('SSH_RESULT:' + line + '\r\n')
        async def start():
            return await asyncssh.create_server(Server, '127.0.0.1', 0, server_host_keys=[self.key], process_factory=shell, line_editor=True)
        self.server = asyncio.run_coroutine_threadsafe(start(), self.loop).result(5); self.port = self.server.get_port()
        self.known = self.path/'known_hosts'; self.write_known(self.key)
        bindir = self.path/'bin'; bindir.mkdir()
        wrapper = bindir/'ssh'; wrapper.write_text('#!/usr/bin/python3\nimport os,sys\nos.execv("/usr/bin/ssh",["ssh",'+repr('-oUserKnownHostsFile='+str(self.known))+',"-oGlobalKnownHostsFile=/dev/null","-F","/dev/null",*sys.argv[1:]])\n'); wrapper.chmod(0o700)
        runtime = self.path/'runtime'; runtime.mkdir(mode=0o700)
        self.env = dict(os.environ, PATH=str(bindir)+os.pathsep+os.environ['PATH'], XDG_RUNTIME_DIR=str(runtime))
        self.addCleanup(self.stop_server)
    def write_known(self, key): self.known.write_bytes(f'[127.0.0.1]:{self.port} '.encode()+key.export_public_key())
    def stop_server(self):
        async def stop(): self.server.close(); await self.server.wait_closed()
        asyncio.run_coroutine_threadsafe(stop(), self.loop).result(5); self.loop.call_soon_threadsafe(self.loop.stop); self.thread.join(2); self.loop.close()
    def connection(self, password=PASSWORD):
        settings = profile(0); settings['port'] = self.port
        conn = Connection(settings, password, self.env); self.addCleanup(conn.close); return conn
    def test_password_authentication_and_resize(self):
        conn = self.connection(); conn.until(b'SSH_READY')
        self.assertEqual(self.auth, [('tester', PASSWORD)]); self.assertNotIn(PASSWORD.encode(), conn.output)
        conn.send('hello\r'); conn.until(b'SSH_RESULT:hello')
        fcntl.ioctl(conn.master, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 110, 0, 0)); os.kill(conn.process.pid, signal.SIGWINCH); time.sleep(.25)
        conn.send('size\r'); conn.until(b'SIZE:110x40'); conn.send('exit\r'); self.assertEqual(conn.finish(), 0)
        self.assertFalse(list((self.path/'runtime').glob('devterm-auth-*')))
    def test_wrong_password_no_retry(self):
        conn = self.connection('wrong-test-password'); self.assertNotEqual(conn.finish(), 0)
        self.assertEqual(self.auth, [('tester', 'wrong-test-password')]); self.assertNotIn(b'wrong-test-password', conn.output)
    def test_changed_host_key_is_rejected_before_password(self):
        self.write_known(asyncssh.generate_private_key('ssh-ed25519')); before = self.known.read_bytes()
        conn = self.connection(); self.assertNotEqual(conn.finish(), 0); self.assertFalse(self.auth)
        self.assertIn(b'HOST IDENTIFICATION HAS CHANGED', conn.output); self.assertEqual(self.known.read_bytes(), before)
    def test_private_password_socket_rejects_unrelated_process(self):
        self.auth_gate.clear(); conn = self.connection()
        end = time.monotonic() + 5; paths = []
        while not paths and time.monotonic() < end:
            paths = list((self.path/'runtime').glob('devterm-auth-*/password.sock')); time.sleep(.01)
        self.assertTrue(paths); self.assertEqual(paths[0].parent.stat().st_mode & 0o777, 0o700)
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2); client.connect(str(paths[0])); self.assertEqual(client.recv(1024), b'')
        self.auth_gate.set(); conn.until(b'SSH_READY'); self.assertEqual(self.auth, [('tester', PASSWORD)])
        conn.send('exit\r'); self.assertEqual(conn.finish(), 0)
    def host_dialog(self):
        end = time.monotonic() + 8
        while time.monotonic() < end:
            result = subprocess.run(['xdotool','search','--onlyvisible','--name','DevTerm — Verify SSH Host Key'], text=True, capture_output=True)
            if result.returncode == 0: return result.stdout.splitlines()[0]
            time.sleep(.05)
        self.fail('SSH host-key dialog did not appear')
    @unittest.skipUnless(os.environ.get('DISPLAY') and shutil.which('xdotool'), 'Requires X11 and xdotool')
    def test_unknown_host_key_cancel_does_not_send_password(self):
        self.known.write_text(''); conn = self.connection(); window = self.host_dialog()
        subprocess.run(['xdotool','windowfocus','--sync',window,'key','Escape'], check=True, capture_output=True)
        self.assertNotEqual(conn.finish(), 0); self.assertFalse(self.auth); self.assertEqual(self.known.read_text(), '')
    @unittest.skipUnless(os.environ.get('DISPLAY') and shutil.which('xdotool'), 'Requires X11 and xdotool')
    def test_unknown_host_key_explicit_trust_then_password(self):
        self.known.write_text(''); conn = self.connection(); window = self.host_dialog()
        geometry = subprocess.check_output(['xdotool','getwindowgeometry','--shell',window], text=True)
        width = int(re.search(r'WIDTH=(\d+)', geometry).group(1)); height = int(re.search(r'HEIGHT=(\d+)', geometry).group(1))
        subprocess.run(['xdotool','mousemove','--window',window,str(width-85),str(height-25),'click','1'], check=True, capture_output=True)
        conn.until(b'SSH_READY'); self.assertEqual(self.auth, [('tester', PASSWORD)])
        self.assertIn(b'ssh-ed25519', self.known.read_bytes()); conn.send('exit\r'); self.assertEqual(conn.finish(), 0)

if __name__ == '__main__': unittest.main(verbosity=2)
