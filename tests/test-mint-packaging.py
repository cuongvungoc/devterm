"""Standalone CLI regression checks; requires Python 3 and dpkg-deb, no Docker."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'packaging/build-mint-deb.py'


class MintPackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='devterm-mint-report-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        stage = self.root / 'stage'
        (stage / 'DEBIAN').mkdir(parents=True)
        (stage / 'DEBIAN/control').write_text(
            'Package: devterm\nVersion: 0.1.0-2~ubuntu24.04\nArchitecture: amd64\n'
            'Maintainer: Test <noreply@example.invalid>\nDepends: libc6\n'
            'Installed-Size: 1\nDescription: Packaging test fixture\n')
        self.payload = b'fixture payload that must remain byte-for-byte unchanged\n'
        binary = stage / 'usr/lib/devterm/devterm'
        binary.parent.mkdir(parents=True)
        binary.write_bytes(self.payload)
        doc = stage / 'usr/share/doc/devterm/README.md'
        doc.parent.mkdir(parents=True)
        doc.write_text('Packaging fixture documentation\n')
        self.source = self.root / 'devterm_0.1.0-2~ubuntu24.04_amd64.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(stage), str(self.source)],
                       check=True, stdout=subprocess.DEVNULL)
        self.report = dict(sha256=hashlib.sha256(self.source.read_bytes()).hexdigest(),
                           test_groups=11, tests_passed=True, skips=False)

    def run_repackage(self, output):
        self.source.with_suffix('.build.json').write_text(json.dumps(self.report))
        return subprocess.run([sys.executable, str(SCRIPT), str(self.source), '--output', str(output)],
                              text=True, capture_output=True)

    def test_original_and_expanded_suites_preserve_payload(self):
        for groups in (9, 11, 12):
            with self.subTest(groups=groups):
                self.report['test_groups'] = groups
                output = self.root / f'output-{groups}'
                result = self.run_repackage(output)
                self.assertEqual(result.returncode, 0, result.stderr)
                artifact = output / 'devterm_0.1.0-2~linuxmint22.1_amd64.deb'
                manifest = json.loads(artifact.with_suffix('.build.json').read_text())
                self.assertEqual(manifest['source_test_groups'], groups)
                self.assertEqual(manifest['source_sha256'], self.report['sha256'])
                archive = subprocess.check_output(['dpkg-deb', '--fsys-tarfile', str(artifact)])
                with tarfile.open(fileobj=io.BytesIO(archive)) as files:
                    self.assertEqual(files.extractfile('./usr/lib/devterm/devterm').read(), self.payload)
                self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(), manifest['sha256'])

    def test_invalid_reports_never_produce_a_package(self):
        cases = [dict(test_groups=n) for n in (0, 8, True, '11', None)]
        cases += [dict(sha256='0' * 64), dict(skips=True), dict(tests_passed=False)]
        for i, fields in enumerate(cases):
            with self.subTest(fields=fields):
                original = dict(self.report)
                self.report.update(fields)
                output = self.root / f'rejected-{i}'
                result = self.run_repackage(output)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('successful, unskipped build report', result.stderr)
                self.assertFalse(list(output.glob('*.deb')))
                self.report = original


if __name__ == '__main__':
    unittest.main()
