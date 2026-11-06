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


def enrollment(source, hub):
    """Return destination payloads without exposing credential contents."""
    source = source.resolve(strict=True)
    files = {}
    for role, source_name, dest in (
        ("agent", "agent.json", ROOT / "config/agent"),
        ("viewer", "client.json", PUBLIC),
    ):
        config_path = source / "config/node" / source_name
        settings = json.loads(read_file(config_path))
        if settings.get("version") != 1:
            raise ValueError("Only enrollment version 1 is supported")
        for key, filename in (("ca_file", "ca.pem"), ("token_file", role + ".token")):
            files[dest / filename] = reference(config_path, settings, key, source)
            settings[key] = filename
        files[dest / source_name] = json_bytes(settings)
    if hub:
        config_path = source / "config/enrollment/server/server.json"
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
    managed = {COMMAND: wrapper()}
    for role in ("server", "agent") if hub else ("agent",):
        managed[UNITS / f"pto-btop-{role}.service"] = unit(role)
    for path in managed:
        if path.exists() or path.is_symlink():
            if MARKER.encode() not in read_file(path).splitlines(keepends=True)[:2]:
                raise ValueError(f"Refusing to overwrite an unmanaged command or unit: {path}")
    return managed


def verify_archive(archive, arch, dest):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != HASHES[arch]:
        raise ValueError("Release SHA-256 mismatch; nothing installed")
    name = f"pto-btop-{VERSION}-linux-{arch}"
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


def fetch_package(args, temporary):
    arch = {"x86_64": "amd64", "aarch64": "arm64"}.get(platform.machine())
    if platform.system() != "Linux" or arch is None:
        raise ValueError("Requires Linux amd64 or arm64")
    if args.archive:
        archive = args.archive
    else:
        archive = temporary / "package.tar.gz"
        url = f"https://github.com/pypto-tools/pto-btop/releases/download/v{VERSION}/pto-btop-{VERSION}-linux-{arch}.tar.gz"
        command = ["curl", "--disable", "--fail", "--silent", "--show-error", "--location",
                   "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "15",
                   "--max-time", "180", "--max-filesize", str(64 * 1024 * 1024), "--output", str(archive), url]
        if args.proxy:
            command += ["--proxy", args.proxy, "--noproxy", ""]
        if subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            raise ValueError("GitHub download failed; check the proxy and HTTPS connection")
    return verify_archive(archive, arch, temporary)


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True, help="Existing private installation, not the config bundle")
    parser.add_argument("--hub", action="store_true", help="Also install the central server on this machine")
    parser.add_argument("--proxy", help="Proxy URL for GitHub package downloads")
    parser.add_argument("--archive", type=Path, help="Use a previously downloaded official archive")
    parser.add_argument("--check", action="store_true", help="Read-only preflight; does not require sudo")
    parser.add_argument("--activate", action="store_true", help="Migrate old services and enable/start system services")
    args = parser.parse_args()
    if args.check and args.activate:
        parser.error("--check and --activate are mutually exclusive")
    if sys.version_info < (3, 9):
        parser.error("Python 3.9 or newer is required")
    if not args.check and os.geteuid() != 0:
        parser.error("Run installation with sudo; --check is available without sudo")
    source = args.source_root.resolve(strict=True)
    if source == ROOT:
        parser.error("Use the original private installation as --source-root")
    roles = ("server", "agent") if args.hub else ("agent",)
    systemd_check()
    files = enrollment(source, args.hub)
    managed = check_targets(files, args.hub)
    processes = {role: existing_background(source, role) for role in roles}
    user_services = [s for role in roles if (s := user_service(source, role))]
    if any(processes.values()):
        os.close(pidfd_open(os.getpid()))
    previous_release = os.readlink(ROOT / "app/current") if (ROOT / "app/current").is_symlink() else None
    previous_units = {path: read_file(path) for path in managed if path.parent == UNITS and path.exists()}
    previously_active = [f"pto-btop-{role}.service" for role in roles
                         if subprocess.run(["systemctl", "is-active", "--quiet", f"pto-btop-{role}.service"],
                                           capture_output=True).returncode == 0]
    previously_enabled = [f"pto-btop-{role}.service" for role in roles
                          if subprocess.run(["systemctl", "is-enabled", "--quiet", f"pto-btop-{role}.service"],
                                            capture_output=True).returncode == 0]
    with tempfile.TemporaryDirectory(prefix="pto-btop-system-") as temporary:
        package = fetch_package(args, Path(temporary))
        installer = runpy.run_path(str(package / "install.py"))
        metadata, digest = installer["validate"](package)
        if args.check:
            print(f"Checks passed: v{VERSION}, {'hub + agent' if args.hub else 'agent'}, all local users may view.")
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
        installer["install"](package, ROOT, LIBEXEC, metadata, digest)
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
            atomic_file(path, data, 0o755 if path == COMMAND else 0o644)
        run("systemd-analyze", "verify", *(str(UNITS / f"pto-btop-{role}.service") for role in roles))
        run("systemctl", "daemon-reload")
        # Exercise the global entrypoint as an unrelated, unprivileged account.
        run("runuser", "-u", "nobody", "--", str(COMMAND), "--version")
        if args.activate:
            try:
                activate(source, roles, processes, user_services)
            except Exception:
                if previous_release:
                    switch = ROOT / "app/.system-rollback"
                    switch.symlink_to(previous_release)
                    os.replace(switch, ROOT / "app/current")
                for path, data in previous_units.items():
                    atomic_file(path, data, 0o644)
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
    print("All local users: /usr/local/bin/pto-btop")
    print("View logs: journalctl -u pto-btop-agent" + (" -u pto-btop-server" if args.hub else ""))
    print("Existing private enrollment was preserved. The public config contains only a read-only viewer token and CA certificate.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from error
