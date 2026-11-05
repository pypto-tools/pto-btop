#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the release's enrollment installer, pinning both package checksums."""
import argparse
import hashlib
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    version = (source / "VERSION").read_text().strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[a-z0-9.]+)?", version):
        raise ValueError("Invalid VERSION")
    digests = {}
    for arch in ("amd64", "arm64"):
        package = args.assets / f"pto-btop-{version}-linux-{arch}.tar.gz"
        digest = hashlib.sha256(package.read_bytes()).hexdigest()
        if package.with_name(package.name + ".sha256").read_text().split() != [digest, package.name]:
            raise ValueError(f"Package checksum mismatch: {arch}")
        digests[arch] = digest
    template = (source / "scripts/node-setup.py.in").read_text()
    script = template.replace("@VERSION@", version).replace("@SHA256@", repr(digests))
    compile(script, "setup.py", "exec")
    target = args.assets / "setup.py"
    with target.open("x") as output:
        output.write(script)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    with (args.assets / "setup.py.sha256").open("x") as output:
        output.write(f"{digest}  setup.py\n")
    print(target)


if __name__ == "__main__":
    main()
