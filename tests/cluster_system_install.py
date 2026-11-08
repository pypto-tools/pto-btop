#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise public-viewer isolation and failure-safe service migration."""
import ast
import importlib.util
import json
import hashlib
import io
from pathlib import Path
import signal
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

SCRIPT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and sys.argv[1].endswith('.py') else Path(__file__).resolve().parents[1] / 'scripts/system-install.py'
spec = importlib.util.spec_from_file_location('system_install', SCRIPT)
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pto-system-test-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / 'system'
        self.public = self.base / 'public'
        self.bin = self.base / 'bin'
        self.units = self.base / 'units'
        self.command = self.base / 'pto-btop'
        patch = mock.patch.multiple(deploy, ROOT=self.root, PUBLIC=self.public,
                                    LIBEXEC=self.bin, UNITS=self.units, COMMAND=self.command,
                                    UPDATE_COMMAND=self.base / 'pto-btop-update')
        patch.start()
        self.addCleanup(patch.stop)
        self.source = self.base / 'source'
        node = self.source / 'config/node'
        node.mkdir(parents=True)
        for role in ('agent', 'viewer'):
            (node / f'{role}.token').write_text(role * 16)
            name = 'agent.json' if role == 'agent' else 'client.json'
            (node / name).write_text(json.dumps({'version': 1, 'server': 'https://127.0.0.1:8443',
                                                'ca_file': 'ca.pem', 'token_file': f'{role}.token'}))
        (node / 'ca.pem').write_text('fixture CA')
        server = self.source / 'config/enrollment/server'
        server.mkdir(parents=True)
        (server / 'key.pem').write_text('fixture private key')
        (server / 'cert.pem').write_text('fixture certificate')
        (server / 'server.json').write_text(json.dumps({
            'version': 1, 'listen': '127.0.0.1', 'port': 8443,
            'cert_file': 'cert.pem', 'key_file': 'key.pem',
            'hosts': [{'id': 'example', 'token_file': '../../node/agent.token'}],
            'viewers': [{'id': 'view', 'hosts': ['example'], 'token_file': '../../node/viewer.token'}],
        }))

    def test_export_preserves_auth_and_excludes_private_material_from_public(self):
        before = {p: p.read_bytes() for p in self.source.rglob('*') if p.is_file()}
        files = deploy.enrollment(self.source, True)
        self.assertEqual({p.name for p in files if p.parent == self.public},
                         {'ca.pem', 'client.json', 'viewer.token'})
        public_bytes = b''.join(data for path, data in files.items() if path.parent == self.public)
        self.assertNotIn(b'fixture private key', public_bytes)
        self.assertNotIn(b'agent' * 16, public_bytes)
        server = json.loads(files[self.root / 'config/server/server.json'])
        self.assertEqual(files[self.root / 'config/server' / server['hosts'][0]['token_file']], b'agent' * 16)
        self.assertEqual(server['listen'], '127.0.0.1')
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_source_reference_cannot_escape_or_follow_symlink(self):
        path = self.source / 'config/node/agent.json'
        data = json.loads(path.read_text())
        outside = self.base / 'outside.token'
        outside.write_text('x' * 64)
        data['token_file'] = str(outside)
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'escapes'):
            deploy.enrollment(self.source, False)
        data['token_file'] = 'linked.token'
        (path.parent / 'linked.token').symlink_to(path.parent / 'agent.token')
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'Symlink'):
            deploy.enrollment(self.source, False)

    def test_update_reuses_system_config_without_original_home(self):
        import shutil
        expected = deploy.enrollment(self.source, True)
        for path, data in expected.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        shutil.rmtree(self.source)
        self.assertEqual(deploy.enrollment(self.root, True, system=True), expected)

    def test_latest_version_does_not_download_package_or_restart(self):
        with mock.patch.object(deploy, 'latest_version', return_value='0.5.0'), \
             mock.patch.object(deploy, 'download') as download, \
             mock.patch.object(deploy.platform, 'machine', return_value='aarch64'):
            result = deploy.fetch_package(SimpleNamespace(archive=None, proxy=None), self.base, '0.5.0')
        self.assertEqual(result, (None, '0.5.0'))
        download.assert_not_called()

    def test_prerelease_and_automatic_downgrade_are_rejected(self):
        args = SimpleNamespace(archive=None, proxy=None)
        reply = subprocess.CompletedProcess(['curl'], 0,
                  stdout='https://github.com/pypto-tools/pto-btop/releases/tag/v0.5.0-alpha.1')
        with mock.patch.object(deploy.subprocess, 'run', return_value=reply):
            with self.assertRaisesRegex(ValueError, 'stable'):
                deploy.latest_version(None)
        with mock.patch.object(deploy, 'latest_version', return_value='0.5.0'), \
             mock.patch.object(deploy.platform, 'machine', return_value='aarch64'):
            with self.assertRaisesRegex(ValueError, 'downgrade'):
                deploy.fetch_package(args, self.base, '0.6.0')

    def test_new_stable_version_downloads_and_checks_matching_archive(self):
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:gz') as archive:
            member = tarfile.TarInfo('pto-btop-0.5.1-linux-arm64/example.txt')
            member.size = 7
            archive.addfile(member, io.BytesIO(b'fixture'))
        data = output.getvalue()
        checksum = hashlib.sha256(data).hexdigest()
        calls = []
        def download(url, path, limit, proxy):
            calls.append(url)
            if url.endswith('.sha256'):
                path.write_text(checksum + '  pto-btop-0.5.1-linux-arm64.tar.gz\n')
            else:
                path.write_bytes(data)
        with mock.patch.object(deploy, 'download', side_effect=download), \
             mock.patch.object(deploy, 'latest_version', return_value='0.5.1'), \
             mock.patch.object(deploy.platform, 'machine', return_value='aarch64'):
            package, version = deploy.fetch_package(SimpleNamespace(archive=None, proxy=None), self.base, '0.5.0')
        self.assertEqual(version, '0.5.1')
        self.assertEqual((package / 'example.txt').read_bytes(), b'fixture')
        self.assertEqual(len(calls), 2)

    def test_refuses_changed_enrollment_and_unknown_commands(self):
        files = deploy.enrollment(self.source, False)
        self.public.mkdir()
        (self.public / 'viewer.token').write_text('different enrollment')
        with mock.patch.object(deploy, 'ordinary_parents'), self.assertRaisesRegex(ValueError, 'differs'):
            deploy.check_targets(files, False)
        (self.public / 'viewer.token').unlink()
        self.command.write_text('#!/bin/sh\necho unrelated command\n')
        with mock.patch.object(deploy, 'ordinary_parents'), self.assertRaisesRegex(ValueError, 'unmanaged'):
            deploy.check_targets(files, False)

    def install_launcher(self, child_body):
        self.public.mkdir()
        self.bin.mkdir()
        for path, data in deploy.enrollment(self.source, False).items():
            if path.parent == self.public:
                path.write_bytes(data)
        binary = self.bin / 'pto-btop'
        binary.write_text(f'#!{sys.executable}\n' + child_body)
        binary.chmod(0o755)
        self.command.write_bytes(deploy.wrapper())
        self.command.chmod(0o755)

    def test_launcher_creates_private_token_and_cleans_up(self):
        self.install_launcher('''import json, os, sys
from pathlib import Path
p=Path(sys.argv[sys.argv.index('--config')+1])
c=json.loads(p.read_text())
token=p.parent/c['token_file']
assert token.stat().st_mode & 0o777 == 0o600
assert p.parent.stat().st_mode & 0o777 == 0o700
assert token.read_text() == 'viewer'*16
print(json.dumps({'directory':str(p.parent), 'args':sys.argv[3:], 'ca':c['ca_file']}))
''')
        result = subprocess.check_output([str(self.command), '--check', '--theme', 'with spaces'], text=True)
        data = json.loads(result)
        self.assertEqual(data['args'], ['--check', '--theme', 'with spaces'])
        self.assertEqual(data['ca'], str(self.public / 'ca.pem'))
        self.assertFalse(Path(data['directory']).exists())

    def test_explicit_modes_bypass_public_credentials(self):
        self.install_launcher('import json,sys\nprint(json.dumps(sys.argv[1:]))\n')
        (self.public / 'viewer.token').unlink()
        for arguments in (['--demo'], ['--local'], ['--ssh', 'dev'], ['--config', 'private file.json'], ['--version']):
            result = subprocess.check_output([str(self.command)] + arguments, text=True)
            self.assertEqual(json.loads(result), arguments)

    def test_launcher_forwards_term_and_cleans_up(self):
        self.install_launcher('''import json,sys,time,signal
from pathlib import Path
p=Path(sys.argv[sys.argv.index('--config')+1])
signal.signal(signal.SIGTERM,lambda *args:sys.exit(0))
print(p.parent,flush=True)
time.sleep(20)
''')
        process = subprocess.Popen([str(self.command)], stdout=subprocess.PIPE, text=True)
        try:
            directory = Path(process.stdout.readline().strip())
            self.assertTrue(directory.exists())
            process.terminate()
            self.assertEqual(process.wait(timeout=5), 0)
            self.assertFalse(directory.exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()

    def test_wrong_process_is_never_stopped(self):
        command = [sys.executable, '-c', 'import time;time.sleep(20)']
        process = subprocess.Popen(command)
        try:
            with self.assertRaisesRegex(ValueError, 'identity'):
                deploy.stop_background({'pid': process.pid, 'command': command + ['wrong']})
            self.assertIsNone(process.poll())
            deploy.stop_background({'pid': process.pid, 'command': command})
            self.assertEqual(process.wait(timeout=3), -signal.SIGTERM)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_activation_failure_restores_stopped_background(self):
        old = {'role': 'server'}
        def command(*args, **kwargs):
            if args == ('systemctl', 'restart', 'pto-btop-agent.service'):
                raise subprocess.CalledProcessError(1, args)
        with mock.patch.object(deploy, 'run', side_effect=command), \
             mock.patch.object(deploy, 'stop_background') as stop, \
             mock.patch.object(deploy, 'restore_background') as restore, \
             mock.patch.object(deploy.subprocess, 'run') as cleanup, \
             mock.patch.object(deploy.time, 'sleep'):
            with self.assertRaises(subprocess.CalledProcessError):
                deploy.activate(self.source, ('server', 'agent'), {'server': old}, [])
            stop.assert_called_once_with(old)
            restore.assert_called_once_with(old)
            cleanup.assert_any_call(['systemctl', 'stop', 'pto-btop-server.service'], check=False)

    def test_selinux_disabled_needs_no_label_tools(self):
        with mock.patch.object(deploy.shutil, 'which', side_effect=lambda name: '/usr/sbin/getenforce' if name == 'getenforce' else None), \
             mock.patch.object(deploy, 'run', return_value=SimpleNamespace(stdout='Disabled\n')) as command:
            self.assertIsNone(deploy.selinux_preflight(self.root / 'app/releases/test'))
        command.assert_called_once_with('/usr/sbin/getenforce', capture_output=True, text=True, timeout=10)

    def test_selinux_enabled_without_getenforce_does_not_silently_skip(self):
        with mock.patch.object(deploy.shutil, 'which', return_value=None), \
             mock.patch.object(Path, 'exists', return_value=True):
            with self.assertRaisesRegex(ValueError, 'getenforce is missing'):
                deploy.selinux_preflight(self.root / 'app/releases/test')

    def test_selinux_preflight_is_read_only_in_enforcing_and_permissive_modes(self):
        def policy(command, **kwargs):
            path = Path(command[-1])
            kind = 'usr_t' if path in (self.root / 'app', self.root / 'app/current') else 'bin_t'
            return SimpleNamespace(returncode=0, stdout=f'system_u:object_r:{kind}:s0\n')
        for mode in ('Enforcing', 'Permissive'):
            with self.subTest(mode=mode), \
                 mock.patch.object(deploy.shutil, 'which', side_effect=lambda name: '/usr/sbin/' + name), \
                 mock.patch.object(deploy, 'run', return_value=SimpleNamespace(stdout=mode)) as command, \
                 mock.patch.object(deploy.subprocess, 'run', side_effect=policy) as query:
                self.assertEqual(deploy.selinux_preflight(self.root / 'app/releases/test'), '/usr/sbin/restorecon')
                self.assertEqual(command.call_count, 1)  # Only getenforce, never relabel or policy writes.
                self.assertTrue(all(call.args[0][:2] == ['/usr/sbin/matchpathcon', '-n'] for call in query.call_args_list))
                self.assertFalse(self.root.exists())

    def test_selinux_home_or_temporary_policy_fails_before_mutation(self):
        for kind, app_is_valid in (('user_home_t', False), ('user_tmp_t', True), ('usr_t', True)):
            def policy(command, **kwargs):
                path = Path(command[-1])
                actual = 'usr_t' if app_is_valid and path in (self.root / 'app', self.root / 'app/current') else kind
                return SimpleNamespace(returncode=0, stdout=f'unconfined_u:object_r:{actual}:s0\n')
            with self.subTest(kind=kind), \
                 mock.patch.object(deploy.shutil, 'which', side_effect=lambda name: '/usr/sbin/' + name), \
                 mock.patch.object(deploy, 'run', return_value=SimpleNamespace(stdout='Enforcing')) as command, \
                 mock.patch.object(deploy.subprocess, 'run', side_effect=policy):
                with self.assertRaisesRegex(ValueError, 'persistent rules once'):
                    deploy.selinux_preflight(self.root / 'app/releases/test')
                self.assertEqual(command.call_count, 1)
                self.assertFalse(self.root.exists())

    def test_selinux_missing_relabel_tool_fails_before_mutation(self):
        with mock.patch.object(deploy.shutil, 'which', side_effect=lambda name: None if name == 'restorecon' else '/usr/sbin/' + name), \
             mock.patch.object(deploy, 'run', return_value=SimpleNamespace(stdout='Enforcing')):
            with self.assertRaisesRegex(ValueError, 'requires restorecon and matchpathcon'):
                deploy.selinux_preflight(self.root / 'app/releases/test')
        self.assertFalse(self.root.exists())

    def test_rollback_restores_link_before_application_labels(self):
        app = self.root / 'app'
        (app / 'releases/old').mkdir(parents=True)
        (app / 'releases/new').mkdir()
        (app / 'current').symlink_to('releases/new')
        config = self.root / 'config'
        config.mkdir()
        token = config / 'agent.token'
        token.write_text('preserved private fixture')
        token.chmod(0o600)
        def relabel(*args, **kwargs):
            self.assertEqual(args, ('/usr/sbin/restorecon', '-RF', str(app)))
            self.assertEqual((app / 'current').readlink(), Path('releases/old'))
        with mock.patch.object(deploy, 'run', side_effect=relabel) as command:
            deploy.restore_release('releases/old', '/usr/sbin/restorecon')
        command.assert_called_once()
        self.assertEqual(token.read_text(), 'preserved private fixture')
        self.assertEqual(token.stat().st_mode & 0o777, 0o600)

    def test_corrupt_package_rejected_before_extraction(self):
        archive = self.base / 'bad.tar.gz'
        archive.write_bytes(b'not the official package')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            deploy.verify_archive(archive, 'arm64', self.base / 'extract')
        self.assertFalse((self.base / 'extract').exists())

    def test_service_is_unprivileged_and_configuration_is_private(self):
        for role in ('server', 'agent'):
            text = deploy.unit(role).decode()
            self.assertIn('User=pto-btop\n', text)
            self.assertIn('WantedBy=multi-user.target\n', text)
            self.assertIn('ProtectHome=read-only\n', text)
            self.assertNotIn('ProtectHome=true', text)
            self.assertNotIn('/etc/pto-btop/client.json', text)

    def test_python_39_syntax(self):
        ast.parse(SCRIPT.read_text(), feature_version=(3, 9))
        ast.parse(deploy.wrapper().decode(), feature_version=(3, 9))

    def test_bootstrap_checksum_matches_installer(self):
        bootstrap = SCRIPT.resolve().parents[1] / 'install.sh'
        if not bootstrap.exists():
            self.skipTest('Run from the repository to verify the bootstrap')
        self.assertIn(hashlib.sha256(SCRIPT.read_bytes()).hexdigest(), bootstrap.read_text())
        subprocess.run(['bash', '-n', str(bootstrap)], check=True)


if __name__ == '__main__':
    unittest.main()
