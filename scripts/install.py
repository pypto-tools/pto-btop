#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify and install an extracted pto-btop package without starting services."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import tempfile


COMMANDS = ("pto-btop", "pto-btop-agent", "pto-btop-server")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(package):
    metadata_path = package / "package.json"
    metadata = json.loads(metadata_path.read_text())
    if metadata.get("format") != 1 or metadata.get("name") != "pto-btop":
        raise ValueError("Unsupported package manifest")
    version = metadata["version"]
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[a-z0-9.]+)?", version):
        raise ValueError("Invalid package version")
    architecture = {"x86_64": "amd64", "aarch64": "arm64"}.get(platform.machine())
    if platform.system() != "Linux" or not architecture or architecture != metadata["architecture"]:
        raise ValueError(f"Package architecture {metadata['architecture']} does not match this machine")
    libc = os.confstr("CS_GNU_LIBC_VERSION").split()[-1]
    if tuple(map(int, libc.split("."))) < tuple(map(int, metadata["minimum_glibc"].split("."))):
        raise ValueError(f"Requires glibc >= {metadata['minimum_glibc']}; found {libc}")
    files, links = metadata["files"], metadata["symlinks"]
    if set(files) & set(links) or "package.json" in files or "package.json" in links:
        raise ValueError("Invalid package inventory")
    expected_links = {"bin/pto-btop": "btop-cluster", "bin/pto-btop-agent": "btop-agent",
                      "bin/pto-btop-server": "btop-server"}
    if links != expected_links:
        raise ValueError("Unexpected executable links")
    for name in files | links:
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or str(relative) != name:
            raise ValueError("Invalid path in package inventory")
        target = package / name
        # Parent directories must not redirect reads/copies outside the package.
        if any(parent.is_symlink() for parent in target.parents if parent != package and package in parent.parents):
            raise ValueError("Symlink directory in package")
        if name in links:
            if not target.is_symlink() or os.readlink(target) != links[name]:
                raise ValueError(f"Invalid symlink: {name}")
        elif target.is_symlink() or not target.is_file() or sha256(target) != files[name]:
            raise ValueError(f"Package checksum mismatch: {name}")
    actual = {p.relative_to(package).as_posix() for p in package.rglob("*")
              if p.is_symlink() or not p.is_dir()}
    if actual != set(files) | set(links) | {"package.json"}:
        raise ValueError("Package contains missing or unexpected files")
    for command in COMMANDS:
        result = subprocess.run([str(package / "bin" / command), "--version"],
                                capture_output=True, text=True, timeout=5)
        if result.returncode or result.stdout.strip() != f"{command} {version}":
            raise ValueError(f"Cannot run {command}; check required runtime libraries: {result.stderr.strip()}")
    return metadata, sha256(metadata_path)


def safe_directory(path):
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise ValueError(f"Expected an ordinary directory: {path}")


def service(root, role):
    config = "node/agent.json" if role == "agent" else "enrollment/server/server.json"
    return (f"[Unit]\nDescription=pto-btop HTTPS {role}\n\n[Service]\n"
            f"ExecStart={root}/app/current/bin/pto-btop-{role} --config {root}/config/{config}\n"
            "Restart=on-failure\nRestartSec=3\nTimeoutStopSec=6\nNoNewPrivileges=true\nUMask=0077\n"
            "\n[Install]\nWantedBy=default.target\n")


def install(package, root, bindir, metadata, digest):
    # Keep paths literal and safe in systemd ExecStart directives.
    for directory in (root, bindir):
        if not re.fullmatch(r"/[A-Za-z0-9_./-]+", str(directory)):
            raise ValueError("Installation paths must be absolute and contain only letters, digits, /, _, ., -")
        for parent in (directory, *directory.parents):
            safe_directory(parent)
    app = root / "app"
    releases = app / "releases"
    for directory in (app, releases, *(root / name for name in ("config", "state", "logs", "tmp"))):
        safe_directory(directory)
    root.mkdir(parents=True, exist_ok=True)
    app.mkdir(exist_ok=True)
    bindir.mkdir(parents=True, exist_ok=True)
    lock = app / ".install.lock"
    fd = os.open(lock, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        current = app / "current"
        if current.exists() and not current.is_symlink():
            raise ValueError("app/current is not a managed symlink")
        if current.is_symlink():
            target = current.resolve()
            if target.parent != releases or not (target / "package.json").is_file():
                raise ValueError("app/current is not a managed release")
        expected = {bindir / command: current / "bin" / command for command in COMMANDS}
        for entry, target in expected.items():
            if entry.is_symlink():
                if os.readlink(entry) != str(target):
                    raise ValueError(f"Refusing to replace another command: {entry}")
            elif entry.exists():
                raise ValueError(f"Refusing to replace another command: {entry}")
        releases.mkdir(exist_ok=True)
        release_id = f"{metadata['version']}-{metadata['architecture']}-{digest[:12]}"
        release = releases / release_id
        safe_directory(release)
        if release.exists():
            # Templates are installed separately from the verified payload.
            if validate(release)[1] != digest:
                raise ValueError("Installed release was modified")
        else:
            with tempfile.TemporaryDirectory(prefix=".install-", dir=releases) as temporary:
                stage = Path(temporary) / "payload"
                shutil.copytree(package, stage, symlinks=True)
                validate(stage)
                stage.rename(release)
        for name in ("config", "state", "logs", "tmp"):
            (root / name).mkdir(mode=0o700, exist_ok=True)
        # Prepare entrypoints and templates before switching the running version.
        for entry, target in expected.items():
            if not entry.is_symlink():
                entry.symlink_to(target)
        units = app / "systemd"
        safe_directory(units)
        units.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".switch-", dir=app) as temporary:
            for role in ("agent", "server"):
                unit = Path(temporary) / f"pto-btop-{role}.service"
                unit.write_text(service(root, role))
                os.replace(unit, units / unit.name)
            switch = Path(temporary) / "current"
            switch.symlink_to(f"releases/{release_id}")
            os.replace(switch, current)
    print(f"Installed pto-btop {metadata['version']} at {root}")
    print(f"Commands: {bindir}/pto-btop, pto-btop-agent, pto-btop-server")
    print(f"Service templates: {app}/systemd (not enabled or started)")
    print("Configuration and state preserved. Restart existing services explicitly after upgrading.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify package and runtime; do not install")
    parser.add_argument("--root", type=Path, default=Path("/home/pypto-tools/pto-btop"))
    parser.add_argument("--bin-dir", type=Path, default=Path("/usr/local/bin"))
    args = parser.parse_args()
    package = Path(__file__).resolve().parent
    metadata, digest = validate(package)
    if args.check:
        linkage = "bundled" if metadata.get("openssl_linkage") == "static" else "system"
        print(f"Verified pto-btop {metadata['version']} ({metadata['architecture']}, glibc >= {metadata['minimum_glibc']}, {linkage} OpenSSL 3)")
    else:
        install(package, Path(os.path.abspath(args.root)), Path(os.path.abspath(args.bin_dir)), metadata, digest)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from error
