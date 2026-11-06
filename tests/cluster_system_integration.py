"""Container-only integration: real released binaries, mock service manager."""
import importlib.util
import os
from pathlib import Path
import platform
import pwd
import runpy
import shutil
import subprocess
import sys
import tempfile
from unittest import mock

assert os.geteuid() == 0 and Path('/container-test').exists(), 'Use the isolated test container'
script = '/container-test/system-install.py'
spec = importlib.util.spec_from_file_location('deploy', script)
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)
original_run = subprocess.run
services = {}
enabled = set()
unit_verified = []
fail_next = set()


def manager(command, *args, **kwargs):
    if 'systemd-analyze' in command:
        for path in command[2:]:
            text = Path(path).read_text()
            assert 'User=pto-btop\n' in text and 'ProtectSystem=strict\n' in text
            unit_verified.append(path)
        return subprocess.CompletedProcess(command, 0)
    if 'systemctl' not in command:
        return original_run(command, *args, **kwargs)
    if '--user' in command:
        return subprocess.CompletedProcess(command, 1)
    action = command[1]
    name = command[-1]
    if action == 'restart' and name in fail_next:
        fail_next.remove(name)
        raise subprocess.CalledProcessError(1, command)
    code = 0
    if action == 'is-active':
        code = 0 if name in services and services[name].poll() is None else 3
    elif action == 'is-enabled':
        code = 0 if name in enabled else 1
    elif action in ('restart', 'start'):
        if name in services:
            services[name].terminate()
            services[name].wait(timeout=5)
        role = name.removeprefix('pto-btop-').removesuffix('.service')
        account = pwd.getpwnam('pto-btop')
        log = open(f'/tmp/new-{role}.log', 'ab')
        services[name] = subprocess.Popen(
            [str(deploy.ROOT / f'app/current/bin/pto-btop-{role}'), '--config',
             str(deploy.ROOT / f'config/{role}/{role}.json')],
            stdin=subprocess.DEVNULL, stdout=log, stderr=log,
            user=account.pw_uid, group=account.pw_gid, extra_groups=[], start_new_session=True)
        log.close()
    elif action == 'enable':
        enabled.update(command[2:])
    elif action == 'disable':
        enabled.discard(name)
    elif action == 'stop':
        if name in services and services[name].poll() is None:
            services[name].terminate()
            services[name].wait(timeout=5)
    elif action != 'daemon-reload':
        raise AssertionError(command)
    result = subprocess.CompletedProcess(command, code)
    if kwargs.get('check') and code:
        raise subprocess.CalledProcessError(code, command)
    return result


old = Path('/opt/private-install')
initial = []
try:
    with tempfile.TemporaryDirectory() as temporary:
        architecture = {'aarch64': 'arm64', 'x86_64': 'amd64'}[platform.machine()]
        package = deploy.verify_archive(Path('/container-test/release.tar.gz'), architecture, Path(temporary))
        installer = runpy.run_path(str(package / 'install.py'))
        metadata, digest = installer['validate'](package)
        installer['install'](package, old, Path('/opt/private-bin'), metadata, digest)
        original_run([sys.executable, '/container-test/https-init.py', '--directory', str(old / 'config/enrollment'),
                      '--listen', '127.0.0.1', '--server-name', '127.0.0.1', '--hosts', 'example'], check=True)
        shutil.copytree(old / 'config/enrollment/nodes/example', old / 'config/node')
        before = {str(p): p.read_bytes() for p in (old / 'config').rglob('*') if p.is_file()}
        for role in ('server', 'agent'):
            config = 'config/enrollment/server/server.json' if role == 'server' else 'config/node/agent.json'
            with (old / f'logs/pto-btop-{role}.log').open('ab') as log:
                process = subprocess.Popen([str(old / f'app/current/bin/pto-btop-{role}'), '--config', str(old / config)],
                                           stdout=log, stderr=log, start_new_session=True)
            initial.append(process)
            (old / f'state/pto-btop-{role}.pid').write_text(str(process.pid))
        with mock.patch.object(deploy, 'systemd_check'), \
             mock.patch.object(sys, 'argv', [script, '--source-root', str(old), '--hub', '--activate',
                                            '--archive', '/container-test/release.tar.gz']), \
             mock.patch.object(subprocess, 'run', side_effect=manager):
            deploy.main()
            deploy.main()  # Re-running an activated deployment must be idempotent.
        assert all(p.wait(timeout=5) == 0 for p in initial)
        assert before == {p: Path(p).read_bytes() for p in before}
        assert enabled == {'pto-btop-server.service', 'pto-btop-agent.service'}
        assert len(unit_verified) == 4
        old.rename('/opt/preserved-private-install')
        update_args = [script, '--update', '--archive', '/container-test/release.tar.gz']
        with mock.patch.object(deploy, 'systemd_check'), \
             mock.patch.object(sys, 'argv', update_args), \
             mock.patch.object(subprocess, 'run', side_effect=manager):
            deploy.main()  # Updating must not require the original home/config.
            previous_state = (deploy.ROOT / 'state/system-deployment.json').read_bytes()
            fail_next.add('pto-btop-agent.service')
            try:
                deploy.main()
            except subprocess.CalledProcessError:
                pass
            else:
                raise AssertionError('Injected restart failure was ignored')
            assert (deploy.ROOT / 'state/system-deployment.json').read_bytes() == previous_state
        original_run([str(deploy.UPDATE_COMMAND), '--help'], check=True, stdout=subprocess.DEVNULL)
        assert set(p.name for p in deploy.PUBLIC.iterdir()) == {'client.json', 'viewer.token', 'ca.pem'}
        account = pwd.getpwnam('pto-btop')
        for role in ('server', 'agent'):
            assert services[f'pto-btop-{role}.service'].poll() is None
            assert Path(f"/proc/{services[f'pto-btop-{role}.service'].pid}").stat().st_uid == account.pw_uid
        for path in (deploy.ROOT / 'config').rglob('*.token'):
            assert path.stat().st_mode & 0o777 == 0o600
            result = original_run(['runuser', '-u', 'nobody', '--', 'test', '-r', str(path)])
            assert result.returncode != 0
        result = original_run(['runuser', '-u', 'nobody', '--', str(deploy.COMMAND), '--once', '--detail'],
                              capture_output=True, text=True, timeout=25)
        assert result.returncode == 0, result.stderr
        assert 'cpu' in result.stdout.lower() and 'proc' in result.stdout.lower()
        assert not list(Path('/tmp').glob('pto-btop-viewer-*'))
        print('PASS: root installation, ordinary-user HTTPS overview/detail, private credentials, legacy migration, updates without original home, restart-failure recovery, and preserved enrollment')
finally:
    for process in list(services.values()) + initial:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
