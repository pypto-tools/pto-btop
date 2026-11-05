# pto-btop

A Linux terminal monitor for up to four machines, using btop's original drawing
primitives, themes and CPU / memory / disks / network / process layout.

Agents send metrics to an HTTPS hub. Each terminal viewer has independent host
selection, process filters and sorting. Any monitored machine can also run a
viewer; one machine runs the hub. NetBird provides connectivity, and the monitor
verifies HTTPS certificates and uses separate agent and viewer credentials.

![Terminal detail view with simulated data](docs/cluster/detail-preview.png)

**Status:** `0.4.0-alpha.1` preview. Local Linux ARM64 collection, three-agent
HTTPS tests and three independent terminal sessions have been tested. Real
three-machine deployment remains pending. Check the [amd64 / arm64 build results](https://github.com/pypto-tools/pto-btop/actions/workflows/release.yml)
for CI validation and [Releases](https://github.com/pypto-tools/pto-btop/releases)
for published packages; a version in the source tree is not a published release.

## Install

Packages are named `pto-btop-VERSION-linux-amd64.tar.gz` and
`pto-btop-VERSION-linux-arm64.tar.gz`. Select the package matching `uname -m`
(`x86_64` → `amd64`, `aarch64` → `arm64`). Each package includes all three programs,
themes, a configuration generator, installation tooling and licenses.

Runtime requirements: Linux with glibc, OpenSSL 3 shared libraries, and Python
3.10+ for installation/configuration. The package records its actual minimum
glibc version in `package.json` and checks compatibility before installation.
Release builds bundle the GCC C++ runtime. These are not musl/Alpine packages.

After downloading a package and its `.sha256` file:

```bash
sha256sum --check pto-btop-0.4.0-alpha.1-linux-arm64.tar.gz.sha256
tar -xzf pto-btop-0.4.0-alpha.1-linux-arm64.tar.gz
cd pto-btop-0.4.0-alpha.1-linux-arm64
python3 install.py --check
python3 install.py --root "$HOME/.local/share/pto-btop" --bin-dir "$HOME/.local/bin"
"$HOME/.local/bin/pto-btop" --local --detail
```

Substitute `amd64` on x86 machines. The example uses a user-owned directory.
Organization defaults are `/home/pypto-tools/pto-btop` and `/usr/local/bin`;
the installing account must have write access to both. Installation does not
enable services or create enrollment credentials. Upgrades preserve configuration
and state; [installation and release guide](docs/cluster/release.md) has details.

## Connect the machines

Use `scripts/https-init.py` from the extracted package, or
`app/current/scripts/https-init.py` after installation, to generate enrollment
for the hub's NetBird address. Give each node only its own configuration bundle.
See the [HTTPS guide](docs/cluster/https.md) for TLS, access control and services.

```bash
pto-btop-server --config /path/to/server/server.json
pto-btop-agent --config /path/to/node/agent.json
pto-btop --config /path/to/node/client.json --check
pto-btop --config /path/to/node/client.json --detail
```

Run the server once on the hub, an agent on each monitored machine, and a viewer
where needed. Run these foreground programs in separate terminals, or explicitly
enable the supplied service templates. Every viewer can switch among its allowed
hosts. `pto-btop --demo --detail` shows simulated data without a server.

The original executable names `btop-cluster`, `btop-agent` and `btop-server` remain
inside the package for compatibility. Public command names use the `pto-` prefix.

## Build and test

Build dependencies: GCC with full required C++23 support (GCC 14+), CMake 3.25+,
Ninja, nlohmann/json 3.11+, OpenSSL 3 development headers, GTest, Python 3.10+,
OpenSSL CLI and binutils. Pinning and build baseline are documented in the
[release guide](docs/cluster/release.md).

```bash
cmake -S . -B build/cluster -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBTOP_CLUSTER=ON \
  -DBTOP_GPU=OFF -DBTOP_LTO=OFF -DBUILD_TESTING=ON \
  -DBTOP_CLUSTER_STATIC_CXX_RUNTIME=ON
cmake --build build/cluster --parallel 2
ctest --test-dir build/cluster --parallel 2 --output-on-failure
python3 scripts/package.py --build-dir build/cluster --output-dir dist
```

Packaging rejects a glibc requirement above 2.36 by default. Building directly on
a newer distribution may therefore work locally but fail the release check. Use
the fixed CI container for that baseline, or deliberately choose another supported
baseline with `--max-glibc` and document it.

## Documentation and origin

- [Layout, controls and local/SSH modes](docs/cluster/prototype.md)
- [HTTPS deployment](docs/cluster/https.md)
- [Installation, upgrades and publishing](docs/cluster/release.md)
- [Architecture and design material](docs/cluster/README.md)
- [Repository operating skill](skills/pto-btop/SKILL.md)

This is an independent derivative of [aristocratos/btop](https://github.com/aristocratos/btop),
based on commit `e28a2701c7208ff00ab780a68287e2204f356ba0` (upstream version string
1.4.8). It is not an official btop release. The upstream license and copyright
notices are retained. See [LICENSE](LICENSE), [source record](docs/cluster/upstream.json),
and [third-party notices](docs/cluster/third-party.md).
