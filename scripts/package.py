#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package a tested native build; never configure or compile implicitly."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def version_tuple(value):
    return tuple(map(int, value.split(".")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("dist"))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--max-glibc", default="2.28")
    parser.add_argument("--allow-shared-openssl", action="store_true",
                        help="Allow a non-release local package depending on system OpenSSL")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    version = (source / "VERSION").read_text().strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[a-z0-9.]+)?", version):
        raise ValueError("Invalid VERSION")
    architecture = {"aarch64": "arm64", "x86_64": "amd64"}.get(platform.machine())
    if platform.system() != "Linux" or not architecture:
        raise ValueError("Packaging requires native Linux amd64 or arm64")
    name = f"pto-btop-{version}-linux-{architecture}"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    archive = args.output_dir / f"{name}.tar.gz"
    digest_file = args.output_dir / f"{name}.tar.gz.sha256"
    if archive.exists() or digest_file.exists():
        raise ValueError("Output already exists; choose a new output directory")
    with tempfile.TemporaryDirectory(prefix="pto-btop-package-") as directory:
        root = Path(directory) / name
        subprocess.run([args.cmake, "--install", str(args.build_dir.resolve()), "--strip",
                        "--component", "btop-cluster", "--prefix", str(root)], check=True,
                       stdout=subprocess.DEVNULL)
        minimum_glibc = "0.0"
        libraries = set()
        aliases = {"pto-btop": "btop-cluster", "pto-btop-agent": "btop-agent",
                   "pto-btop-server": "btop-server"}
        for public, original in aliases.items():
            binary = root / "bin" / original
            machine = re.search(r"Machine:\s*(.+)", run("readelf", "-h", str(binary)))
            expected_machine = "AArch64" if architecture == "arm64" else "Advanced Micro Devices X86-64"
            if not machine or machine.group(1).strip() != expected_machine:
                raise ValueError(f"ELF architecture does not match the native package: {original}")
            if run(str(binary), "--version") != f"{public} {version}":
                raise ValueError(f"Rebuild {original}: version does not match VERSION")
            dynamic = run("readelf", "-d", str(binary))
            needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
            if any(x in needed for x in ("libstdc++.so.6", "libgcc_s.so.1")):
                raise ValueError("Configure with -DBTOP_CLUSTER_STATIC_CXX_RUNTIME=ON")
            libraries.update(needed)
            versions = re.findall(r"\bGLIBC_([0-9]+\.[0-9]+)",
                                  run("readelf", "--version-info", str(binary)))
            minimum_glibc = max([minimum_glibc, *versions], key=version_tuple)
            (root / "bin" / public).symlink_to(original)
        if version_tuple(minimum_glibc) > version_tuple(args.max_glibc):
            raise ValueError(f"Build requires glibc {minimum_glibc}; maximum is {args.max_glibc}")
        runtime = json.loads((root / "share/btop-cluster/package-runtime.json").read_text())
        shared_openssl = any(name.startswith(("libssl.so", "libcrypto.so")) for name in libraries)
        if shared_openssl != (runtime["openssl_linkage"] == "shared"):
            raise ValueError("OpenSSL linkage disagrees with the installed build metadata")
        if shared_openssl and not args.allow_shared_openssl:
            raise ValueError("Configure with -DBTOP_CLUSTER_STATIC_OPENSSL=ON for release packages")
        shutil.copy2(source / "scripts/install.py", root / "install.py")
        shutil.copy2(source / "README.md", root / "README.md")
        shutil.copy2(source / "VERSION", root / "VERSION")
        shutil.copy2(source / "LICENSE", root / "LICENSE")
        shutil.copytree(source / "skills/pto-btop", root / "skills/pto-btop")
        scripts = root / "scripts"
        scripts.mkdir()
        shutil.copy2(source / "scripts/https-init.py", scripts / "https-init.py")
        docs = root / "docs/cluster"
        docs.mkdir(parents=True)
        for doc in ("release.md", "https.md", "prototype.md", "detail-preview.png", "upstream.json",
                    "README.md", "design.md", "deployment.md", "implementation.md",
                    "centralized-https.md", "protocol.md", "source-study.md", "third-party.md"):
            shutil.copy2(source / "docs/cluster" / doc, docs / doc)
        shutil.copytree(source / "docs/cluster/examples", docs / "examples")
        licenses = root / "share/btop-cluster/licenses"
        for license_name in ("COPYING3", "COPYING.RUNTIME"):
            shutil.copy2(source / "include/gcc-runtime" / license_name, licenses / license_name)
        shutil.copy2(source / "docs/cluster/third-party.md", licenses / "third-party.md")
        manifest = {"format": 1, "name": "pto-btop", "version": version,
                    "architecture": architecture, "minimum_glibc": minimum_glibc,
                    "openssl_major": 3, **runtime, "libraries": sorted(libraries),
                    "files": {}, "symlinks": {}}
        for path in sorted(root.rglob("*")):
            relative = path.relative_to(root).as_posix()
            if path.is_symlink():
                manifest["symlinks"][relative] = os.readlink(path)
            elif path.is_file():
                manifest["files"][relative] = checksum(path)
        (root / "package.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # Validate the same payload an installing user receives.
        subprocess.run([sys.executable, str(root / "install.py"), "--check"], check=True)
        temporary_archive = Path(directory) / archive.name
        with tarfile.open(temporary_archive, "w:gz") as output:
            output.add(root, arcname=name)
        digest = checksum(temporary_archive)
        shutil.copy2(temporary_archive, archive)
        digest_file.write_text(f"{digest}  {archive.name}\n")
    print(archive)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from error
