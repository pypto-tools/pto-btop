#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Install the stable release for all local users; reuse existing enrollment.

Python 3.9+, Linux, systemd. Installation and activation are separate unless
--activate is specified. Credentials are never downloaded or printed.
"""
import argparse
import ctypes
import grp
import hashlib
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import platform
import pwd
import re
import runpy
import select
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time

VERSION = "0.5.0"
HASHES = {
    "amd64": "fa5f88cc4ce1cf17b62a1adb2e6a5d983267e76f7206569210a7b4400a06b228",
    "arm64": "b0ab5f52bbc27a15e5467c7264381e6b7ee2923491da1e640c790ad5fc467e4b",
}
ROOT = Path("/home/pypto-tools/pto-btop")
PUBLIC = Path("/etc/pto-btop")
COMMAND = Path("/usr/local/bin/pto-btop")
UPDATE_COMMAND = Path("/usr/local/bin/pto-btop-update")
LIBEXEC = Path("/usr/local/libexec/pto-btop")
UNITS = Path("/etc/systemd/system")
ACCOUNT = "pto-btop"
MARKER = "# Managed by pto-btop system-install.py\n"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def read_file(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
        raise ValueError(f"Expected a regular file of at most 1 MiB: {path}")
    return path.read_bytes()


def reference(config_path, settings, name, source):
    path = config_path.parent / settings[name]
    resolved = path.resolve(strict=True)
    if source not in resolved.parents:
        raise ValueError(f"Configuration reference escapes the source root: {name}")
    # Resolve relative '..' normally, but never follow a credential symlink.
    if path.is_symlink() or any(p.is_symlink() for p in path.parents):
        raise ValueError(f"Symlink in configuration reference: {name}")
    return read_file(resolved)


def json_bytes(value):
    return (json.dumps(value, indent=2) + "\n").encode()


def enrollment(source, hub, system=False):
    """Return destination payloads without exposing credential contents."""
    source = source.resolve(strict=True)
    files = {}
    for role, source_name, dest in (
        ("agent", "agent.json", ROOT / "config/agent"),
        ("viewer", "client.json", PUBLIC),
    ):
        config_path = (PUBLIC / source_name if role == "viewer" else source / "config/agent" / source_name) if system else source / "config/node" / source_name
        settings = json.loads(read_file(config_path))
        if settings.get("version") != 1:
            raise ValueError("Only enrollment version 1 is supported")
        for key, filename in (("ca_file", "ca.pem"), ("token_file", role + ".token")):
            boundary = PUBLIC if system and role == "viewer" else source
            files[dest / filename] = reference(config_path, settings, key, boundary)
            settings[key] = filename
        files[dest / source_name] = json_bytes(settings)
    if hub:
        config_path = source / ("config/server/server.json" if system else "config/enrollment/server/server.json")
        settings = json.loads(read_file(config_path))
        if ipaddress.ip_address(settings["listen"]).is_unspecified:
            raise ValueError("Keep the hub bound to its existing specific NetBird address")
        dest = ROOT / "config/server"
        for key, filename in (("cert_file", "cert.pem"), ("key_file", "key.pem")):
            files[dest / filename] = reference(config_path, settings, key, source)
            settings[key] = filename
        for group, prefix in (("hosts", "agent"), ("viewers", "viewer")):
            for index, item in enumerate(settings[group]):
                name = f"{prefix}-{index}.token"
                files[dest / name] = reference(config_path, item, "token_file", source)
                item["token_file"] = name
        files[dest / "server.json"] = json_bytes(settings)
    return files


def wrapper():
    binary = str(LIBEXEC / "pto-btop")
    return ("#!/usr/bin/python3\n" + MARKER + f'''import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

binary = {binary!r}
arguments = sys.argv[1:]
if any(a in ("--config", "--demo", "--local", "--ssh", "--help", "--version") for a in arguments):
    os.execv(binary, [binary] + arguments)
with tempfile.TemporaryDirectory(prefix="pto-btop-viewer-") as temporary:
    directory = Path(temporary)
    config = json.loads(Path({str(PUBLIC / 'client.json')!r}).read_text())
    config["ca_file"] = {str(PUBLIC / 'ca.pem')!r}
    config["token_file"] = "viewer.token"
    token = directory / "viewer.token"
    descriptor = os.open(token, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as handle:
        handle.write(Path({str(PUBLIC / 'viewer.token')!r}).read_bytes())
    private_config = directory / "client.json"
    private_config.write_text(json.dumps(config))
    child = subprocess.Popen([binary, "--config", str(private_config)] + arguments)
    def forward(number, frame):
        try:
            child.send_signal(number)
        except ProcessLookupError:
            pass
    signal.signal(signal.SIGINT, forward)
    signal.signal(signal.SIGTERM, forward)
    try:
        result = child.wait()
    finally:
        if child.poll() is None:
            child.terminate()
            child.wait(timeout=10)
sys.exit(result if result >= 0 else 128 - result)
''').encode()


def unit(role):
    return (MARKER + "[Unit]\n" +
            f"Description=pto-btop {role}\n"
            "Wants=network-online.target\nAfter=network-online.target netbird.service\n\n"
            "[Service]\nType=simple\n" +
            f"User={ACCOUNT}\nGroup={ACCOUNT}\n" +
            f"ExecStart={ROOT}/app/current/bin/pto-btop-{role} --config {ROOT}/config/{role}/{role}.json\n"
            "Restart=on-failure\nRestartSec=3\nTimeoutStopSec=10\nUMask=0077\n"
            "NoNewPrivileges=true\nPrivateTmp=true\nProtectSystem=strict\n"
            "ProtectHome=read-only\nCapabilityBoundingSet=\nRestrictSUIDSGID=true\n"
            "Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n"
            "\n[Install]\nWantedBy=multi-user.target\n").encode()


def update_wrapper():
    return ("#!/bin/sh\n" + MARKER +
            f'exec /usr/bin/python3 "{ROOT}/admin/system-install.py" --update "$@"\n').encode()


def ordinary_parents(path):
    for parent in (path, *path.parents):
        if parent.is_symlink() or (parent.exists() and not parent.is_dir()):
            raise ValueError(f"Expected an ordinary directory: {parent}")
        if parent.exists() and (parent.stat().st_uid != 0 or parent.stat().st_mode & 0o022):
            raise ValueError(f"System destination must be root-owned and not writable by others: {parent}")


def check_targets(files, hub):
    for folder in (ROOT, PUBLIC, LIBEXEC, COMMAND.parent, UNITS):
        ordinary_parents(folder)
    for path, data in files.items():
        ordinary_parents(path.parent)
        if path.exists() or path.is_symlink():
            if read_file(path) != data:
                raise ValueError(f"Existing enrollment differs; refusing to replace: {path}")
    managed = {COMMAND: wrapper(), UPDATE_COMMAND: update_wrapper()}
    for role in ("server", "agent") if hub else ("agent",):
        managed[UNITS / f"pto-btop-{role}.service"] = unit(role)
    for path in managed:
        if path.exists() or path.is_symlink():
            if MARKER.encode() not in read_file(path).splitlines(keepends=True)[:2]:
                raise ValueError(f"Refusing to overwrite an unmanaged command or unit: {path}")
    return managed


def verify_archive(archive, arch, dest, version=VERSION, digest=None):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != (digest or HASHES[arch]):
        raise ValueError("Release SHA-256 mismatch; nothing installed")
    name = f"pto-btop-{version}-linux-{arch}"
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            p = PurePosixPath(member.name)
            if p.is_absolute() or ".." in p.parts or not p.parts or p.parts[0] != name:
                raise ValueError("Invalid archive path")
            if not (member.isdir() or member.isfile() or member.issym()):
                raise ValueError("Invalid archive member")
            if member.issym() and (p.parent != PurePosixPath(name) / "bin" or
                                   member.linkname not in ("btop-cluster", "btop-agent", "btop-server")):
                raise ValueError("Invalid archive link")
        # The exact release hash and all archive paths were verified above.
        # Avoid filter=, which is unavailable on older Python 3.9 installations.
        package.extractall(dest)
    return dest / name


def download(url, destination, limit, proxy):
    command = ["curl", "--disable", "--fail", "--silent", "--show-error", "--location",
               "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "15",
               "--max-time", "180", "--retry", "2", "--retry-delay", "1",
               "--max-filesize", str(limit), "--output", str(destination), "--url", url]
    if proxy:
        command += ["--proxy", proxy, "--noproxy", ""]
    status = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    if status:
        raise ValueError(f"GitHub download failed (curl exit {status}); check the proxy and HTTPS connection")
    if destination.stat().st_size > limit:
        raise ValueError("Downloaded file exceeds its size limit")


def latest_version(proxy):
    # The public Latest redirect avoids the low unauthenticated API rate limit.
    command = ["curl", "--disable", "--fail", "--silent", "--show-error", "--location",
               "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "15",
               "--max-time", "60", "--retry", "2", "--retry-delay", "1",
               "--max-filesize", str(2 * 1024 * 1024), "--output", os.devnull,
               "--write-out", "%{url_effective}", "--url", "https://github.com/pypto-tools/pto-btop/releases/latest"]
    if proxy:
        command += ["--proxy", proxy, "--noproxy", ""]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if result.returncode:
        raise ValueError(f"Cannot resolve GitHub Latest (curl exit {result.returncode}); check the proxy")
    match = re.fullmatch(r"https://github\.com/pypto-tools/pto-btop/releases/tag/v([0-9]+\.[0-9]+\.[0-9]+)", result.stdout.strip())
    if not match:
        raise ValueError("GitHub Latest did not resolve to a stable release in this repository")
    return match.group(1)


def fetch_package(args, temporary, installed_version=None):
    arch = {"x86_64": "amd64", "aarch64": "arm64"}.get(platform.machine())
    if platform.system() != "Linux" or arch is None:
        raise ValueError("Requires Linux amd64 or arm64")
    if args.archive:
        archive = args.archive
        version, digest = VERSION, HASHES[arch]
    else:
        version = latest_version(args.proxy)
        tag = 'v' + version
        name = f"pto-btop-{version}-linux-{arch}.tar.gz"
        if installed_version and tuple(map(int, version.split('.'))) < tuple(map(int, installed_version.split('.'))):
            raise ValueError("Latest is older than the installed version; refusing automatic downgrade")
        if version == installed_version and not getattr(args, 'force', False) and not getattr(args, 'check', False):
            return None, version
        base = f"https://github.com/pypto-tools/pto-btop/releases/download/{tag}/"
        checksum = temporary / "package.sha256"
        download(base + name + ".sha256", checksum, 4096, args.proxy)
        fields = checksum.read_text().split()
        if len(fields) != 2 or fields[1] != name or not re.fullmatch(r"[a-f0-9]{64}", fields[0]):
            raise ValueError("Invalid release checksum file")
        digest = fields[0]
        archive = temporary / "package.tar.gz"
        download(base + name, archive, 64 * 1024 * 1024, args.proxy)
    return verify_archive(archive, arch, temporary, version, digest), version


def atomic_file(path, data, mode, gid=0, uid=0):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=".pto-btop-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
            os.fchmod(handle.fileno(), mode)
            os.fchown(handle.fileno(), uid, gid)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def existing_background(source, role):
    pidfile = source / "state" / f"pto-btop-{role}.pid"
    if not pidfile.exists():
        return None
    pid = int(read_file(pidfile).strip())
    expected = [str(source / f"app/current/bin/pto-btop-{role}"), "--config",
                str(source / ("config/enrollment/server/server.json" if role == "server" else "config/node/agent.json"))]
    path = Path(f"/proc/{pid}")
    if not path.exists():
        return None
    if (path / "cmdline").read_bytes().split(b"\0")[:-1] != [a.encode() for a in expected]:
        raise ValueError(f"Recorded {role} PID belongs to another command; refusing migration")
    return {"pid": pid, "command": expected, "role": role, "uid": path.stat().st_uid,
            "pidfile": pidfile, "source": source}


def pidfd_call(number, *arguments):
    syscall = ctypes.CDLL(None, use_errno=True).syscall
    syscall.restype = ctypes.c_long
    result = syscall(ctypes.c_long(number), *(ctypes.c_long(value) for value in arguments))
    if result < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return result


def pidfd_open(pid):
    return os.pidfd_open(pid) if hasattr(os, "pidfd_open") else pidfd_call(434, pid, 0)


def stop_background(process):
    pid = process["pid"]
    try:
        descriptor = pidfd_open(pid)
    except ProcessLookupError:
        return
    try:
        try:
            actual = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")[:-1]
        except FileNotFoundError:
            return
        if actual != [a.encode() for a in process["command"]]:
            raise ValueError("Process identity changed; refusing to stop it")
        if hasattr(signal, "pidfd_send_signal"):
            signal.pidfd_send_signal(descriptor, signal.SIGTERM)
        else:
            pidfd_call(424, descriptor, signal.SIGTERM, 0, 0)
        if not select.select([descriptor], [], [], 10)[0]:
            raise ValueError("Old process did not stop; inspect its log")
    finally:
        os.close(descriptor)


def restore_background(process):
    owner = pwd.getpwuid(process["uid"])
    log = process["source"] / "logs" / f"pto-btop-{process['role']}.log"
    with log.open("ab") as handle:
        child = subprocess.Popen(process["command"], stdin=subprocess.DEVNULL, stdout=handle, stderr=handle,
                                 start_new_session=True, close_fds=True, user=owner.pw_uid,
                                 group=owner.pw_gid, extra_groups=os.getgrouplist(owner.pw_name, owner.pw_gid), umask=0o077)
    process["pidfile"].write_text(f"{child.pid}\n")


def user_service(source, role):
    owner = pwd.getpwuid(source.stat().st_uid)
    command = ([] if os.geteuid() == owner.pw_uid else ["runuser", "-u", owner.pw_name, "--"]) + ["env",
               f"XDG_RUNTIME_DIR=/run/user/{owner.pw_uid}",
               f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{owner.pw_uid}/bus", "systemctl", "--user"]
    name = f"pto-btop-{role}.service"
    enabled = subprocess.run(command + ["is-enabled", "--quiet", name], capture_output=True).returncode == 0
    active = subprocess.run(command + ["is-active", "--quiet", name], capture_output=True).returncode == 0
    if not (enabled or active):
        return None
    unit_path = Path(owner.pw_dir) / ".config/systemd/user" / name
    expected = f"ExecStart={source}/app/current/bin/pto-btop-{role} --config {source}/config/"
    if expected.encode() not in read_file(unit_path):
        raise ValueError(f"Existing user service is not managed by this installation: {name}")
    return {"command": command, "name": name, "active": active, "enabled": enabled}


def activate(source, roles, processes, user_services):
    stopped = []
    disabled = []
    started = []
    try:
        for service in user_services:
            run(*service["command"], "disable", "--now", service["name"])
            disabled.append(service)
        for role in roles:
            process = processes.get(role)
            if process:
                stop_background(process)
                stopped.append(process)
            name = f"pto-btop-{role}.service"
            started.append(name)
            run("systemctl", "restart", name)
            time.sleep(1)
            run("systemctl", "is-active", "--quiet", name)
        run("systemctl", "enable", *started)
    except Exception:
        for name in reversed(started):
            subprocess.run(["systemctl", "stop", name], check=False)
        for process in reversed(stopped):
            restore_background(process)
        for service in disabled:
            if service["enabled"]:
                run(*service["command"], "enable", service["name"])
            if service["active"]:
                run(*service["command"], "start", service["name"])
        raise
    for process in stopped:
        process["pidfile"].unlink(missing_ok=True)


def systemd_check():
    if Path("/proc/1/comm").read_text().strip() != "systemd":
        raise ValueError("This deployment requires systemd as PID 1")


def selinux_preflight(release):
    """Check destination policy without changing labels or SELinux settings."""
    getenforce = shutil.which("getenforce")
    if not getenforce:
        if Path("/sys/fs/selinux/enforce").exists():
            raise ValueError("SELinux is enabled but getenforce is missing; install SELinux utilities")
        return None
    mode = run(getenforce, capture_output=True, text=True, timeout=10).stdout.strip()
    if mode == "Disabled":
        return None
    if mode not in ("Enforcing", "Permissive"):
        raise ValueError("Cannot determine SELinux mode; no application files were changed")
    restorecon, matchpathcon = shutil.which("restorecon"), shutil.which("matchpathcon")
    if not restorecon or not matchpathcon:
        raise ValueError("SELinux deployment requires restorecon and matchpathcon; install SELinux utilities")
    expected = {ROOT / "app": "usr_t", ROOT / "app/current": "usr_t", release / "bin": "bin_t"}
    for command in ("btop-cluster", "btop-agent", "btop-server", "pto-btop", "pto-btop-agent", "pto-btop-server"):
        expected[release / "bin" / command] = "bin_t"
    for path, kind in expected.items():
        result = subprocess.run([matchpathcon, "-n", str(path)], capture_output=True, text=True, timeout=10)
        context = result.stdout.strip().split(":")
        if result.returncode or len(context) < 4 or context[2] != kind:
            raise ValueError(
                f"SELinux destination policy must label {path} as {kind}. "
                "Configure these persistent rules once, in this order, then retry:\n"
                f"sudo semanage fcontext -a -t usr_t '{ROOT}/app(/.*)?'\n"
                f"sudo semanage fcontext -a -t bin_t '{ROOT}/app/releases/[^/]+/bin(/.*)?'\n"
                "If a rule already exists, inspect it instead of replacing it blindly. "
                "Application files and services have not been changed.")
    return restorecon


def restore_application_labels(restorecon):
    if restorecon:
        # copytree preserves source xattrs, including user_tmp_t from extraction.
        # Relabel only application files; credentials, state and other tools stay outside this tree.
        run(restorecon, "-RF", str(ROOT / "app"), timeout=60)


def restore_release(previous_release, restorecon):
    if previous_release:
        switch = ROOT / "app/.system-rollback"
        switch.symlink_to(previous_release)
        os.replace(switch, ROOT / "app/current")
        restore_application_labels(restorecon)


def default_source():
    home = Path(pwd.getpwnam(os.environ["SUDO_USER"]).pw_dir) if os.environ.get("SUDO_USER") else Path.home()
    return home / ".local/share/pto-btop"


def deployment_state():
    path = ROOT / "state/system-deployment.json"
    if not path.exists():
        return None
    ordinary_parents(path.parent)
    if path.stat().st_uid != 0 or path.stat().st_mode & 0o077:
        raise ValueError("System deployment state must be root-owned and private")
    data = json.loads(read_file(path))
    if data.get("format") != 1 or not isinstance(data.get("hub"), bool) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", data.get("version", "")):
        raise ValueError("Invalid system deployment state")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, help="Existing private installation; auto-detected for the invoking account")
    role = parser.add_mutually_exclusive_group()
    role.add_argument("--hub", action="store_const", const=True, default=None, dest="hub", help="Install the central server too; normally auto-detected")
    role.add_argument("--node", action="store_const", const=False, dest="hub", help="Install only the agent and public viewer")
    parser.add_argument("--update", action="store_true", help="Update the managed system deployment to GitHub Latest and restart")
    parser.add_argument("--force", action="store_true", help="Reinstall/restart even when already on the latest release")
    parser.add_argument("--proxy", help="Proxy URL for GitHub package downloads")
    parser.add_argument("--archive", type=Path, help="Offline installation using the pinned official v0.5.0 archive")
    parser.add_argument("--check", action="store_true", help="Read-only preflight; does not require sudo")
    parser.add_argument("--activate", action="store_true", help="Migrate old services and enable/start system services")
    args = parser.parse_args()
    if args.check and args.activate:
        parser.error("--check and --activate are mutually exclusive")
    if sys.version_info < (3, 9):
        parser.error("Python 3.9 or newer is required")
    if not args.check and os.geteuid() != 0:
        parser.error("Run installation with sudo; --check is available without sudo")
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "aarch64"):
        raise ValueError("Requires Linux amd64 or arm64")
    systemd_check()
    state = deployment_state()
    if args.update and state is None:
        parser.error("No managed deployment found; run the one-command installer first")
    updating = state is not None
    if args.update and not args.check:
        args.activate = True
    source = ROOT if updating else (args.source_root or default_source()).resolve(strict=True)
    if updating:
        if args.hub is not None and args.hub != state['hub']:
            raise ValueError("The requested role differs from the installed system deployment")
        hub = state['hub']
    else:
        hub = args.hub if args.hub is not None else (source / 'config/enrollment/server/server.json').is_file()
        if source == ROOT:
            raise ValueError("System state is missing; inspect the existing system deployment")
    roles = ("server", "agent") if hub else ("agent",)
    files = enrollment(source, hub, system=updating)
    managed = check_targets(files, hub)
    legacy = None if updating and state.get('activated') else Path(state['legacy_source']) if updating else source
    processes = {role: existing_background(legacy, role) for role in roles} if legacy else {}
    user_services = [s for role in roles if (s := user_service(legacy, role))] if legacy else []
    if any(processes.values()):
        os.close(pidfd_open(os.getpid()))
    previous_release = os.readlink(ROOT / "app/current") if (ROOT / "app/current").is_symlink() else None
    previous_units = {path: read_file(path) for path in managed if path.parent == UNITS and path.exists()}
    admin = ROOT / 'admin/system-install.py'
    ordinary_parents(admin.parent)
    if admin.exists() and not updating:
        raise ValueError("Refusing to replace an unmanaged administrator entrypoint")
    previous_admin = read_file(admin) if admin.exists() else None
    previously_active = [f"pto-btop-{role}.service" for role in roles
                         if subprocess.run(["systemctl", "is-active", "--quiet", f"pto-btop-{role}.service"],
                                           capture_output=True).returncode == 0]
    previously_enabled = [f"pto-btop-{role}.service" for role in roles
                          if subprocess.run(["systemctl", "is-enabled", "--quiet", f"pto-btop-{role}.service"],
                                            capture_output=True).returncode == 0]
    with tempfile.TemporaryDirectory(prefix="pto-btop-system-") as temporary:
        installed_version = state['version'] if updating else None
        if updating and not state.get('activated'):
            args.force = True
        package, version = fetch_package(args, Path(temporary), installed_version)
        if package is None:
            print(f"Already on the latest stable release ({version}); services unchanged.")
            return
        installer = runpy.run_path(str(package / "install.py"))
        metadata, digest = installer["validate"](package)
        if metadata['version'] != version:
            raise ValueError("Release version does not match the package manifest")
        release = ROOT / 'app/releases' / f"{version}-{metadata['architecture']}-{digest[:12]}"
        restorecon = selinux_preflight(release)
        if args.check:
            print(f"Checks passed: v{version}, {'hub + agent' if hub else 'agent'}, all local users may view.")
            print("Enrollment, processes and system files unchanged. Re-run with sudo and --activate to deploy.")
            return
        os.umask(0o022)
        try:
            account = pwd.getpwnam(ACCOUNT)
            if account.pw_uid == 0 or account.pw_shell not in ("/usr/sbin/nologin", "/sbin/nologin", "/bin/false"):
                raise ValueError("Existing pto-btop account is not a dedicated non-login service account")
        except KeyError:
            run("useradd", "--system", "--user-group", "--home-dir", "/nonexistent", "--no-create-home",
                "--shell", shutil.which("nologin") or "/sbin/nologin", ACCOUNT)
            account = pwd.getpwnam(ACCOUNT)
        if grp.getgrnam(ACCOUNT).gr_gid != account.pw_gid:
            raise ValueError("The pto-btop service account must use its own primary group")
        try:
            installer["install"](package, ROOT, LIBEXEC, metadata, digest)
            restore_application_labels(restorecon)
        except Exception:
            restore_release(previous_release, restorecon)
            raise
        # The verified payload was extracted under a private temporary directory;
        # its installed parent paths must be traversable by ordinary viewers.
        for path in (ROOT, ROOT / "app", ROOT / "app/releases", LIBEXEC, PUBLIC):
            path.mkdir(parents=True, exist_ok=True)
            path.chmod(0o755)
        private = ROOT / "config"
        private.chmod(0o750)
        os.chown(private, 0, account.pw_gid)
        for path, data in files.items():
            public = path.parent == PUBLIC
            path.parent.mkdir(parents=True, exist_ok=True)
            if not public:
                path.parent.chmod(0o750)
                os.chown(path.parent, 0, account.pw_gid)
            atomic_file(path, data, 0o644 if public else 0o600,
                        0 if public else account.pw_gid, 0 if public else account.pw_uid)
        for path, data in managed.items():
            atomic_file(path, data, 0o755 if path in (COMMAND, UPDATE_COMMAND) else 0o644)
        # Releases may carry an updated administrator entrypoint. v0.5.0 predates
        # this helper, so bootstrap it from this checksum-verified installer.
        released_helper = package / 'scripts/system-install.py'
        helper = read_file(released_helper) if released_helper.is_file() and '--update' in released_helper.read_text() else Path(__file__).read_bytes()
        atomic_file(admin, helper, 0o755)
        run("systemd-analyze", "verify", *(str(UNITS / f"pto-btop-{role}.service") for role in roles))
        run("systemctl", "daemon-reload")
        # Exercise the global entrypoint as an unrelated, unprivileged account.
        run("runuser", "-u", "nobody", "--", str(COMMAND), "--version")
        if args.activate:
            try:
                activate(source, roles, processes, user_services)
            except Exception:
                restore_release(previous_release, restorecon)
                for path, data in previous_units.items():
                    atomic_file(path, data, 0o644)
                if previous_admin is not None:
                    atomic_file(admin, previous_admin, 0o755)
                for role in roles:
                    name = f"pto-btop-{role}.service"
                    if name not in previously_enabled:
                        subprocess.run(["systemctl", "disable", name], check=False)
                run("systemctl", "daemon-reload")
                for name in previously_active:
                    run("systemctl", "start", name)
                raise
            result = subprocess.run(["runuser", "-u", "nobody", "--", str(COMMAND), "--check"])
            if result.returncode:
                print("Services started; not all monitored hosts are ONLINE yet. Run pto-btop --check after their agents reconnect.")
        else:
            print("Installed only. Re-run with --activate to migrate and start the system services.")
        atomic_file(ROOT / 'state/system-deployment.json', json_bytes({
            'format': 1, 'hub': hub, 'version': version,
            'activated': bool(args.activate or (state and state.get('activated'))),
            'legacy_source': str(legacy) if legacy else None,
        }), 0o600)
    print("All local users: /usr/local/bin/pto-btop")
    print("Next update: sudo pto-btop-update [--proxy URL]")
    print("View logs: journalctl -u pto-btop-agent" + (" -u pto-btop-server" if hub else ""))
    print("Existing private enrollment was preserved. The public config contains only a read-only viewer token and CA certificate.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from error
