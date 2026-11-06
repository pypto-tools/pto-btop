---
name: pto-btop
description: Install, configure, upgrade or diagnose pto-btop terminal monitoring over HTTPS. Use for this tool's server, agents and viewers; not for unrelated NetBird configuration or system administration.
---

Read [release and installation instructions](../../docs/cluster/release.md) for packages and upgrades, and [HTTPS instructions](../../docs/cluster/https.md) for enrollment and data flow.

- Read-only checks: `uname -m`, `getconf GNU_LIBC_VERSION`, package `install.py --check`, program `--version`, and `pto-btop --config PATH --check`. Describe stale/disconnected data using its timestamp. A local multi-agent test is not evidence of deployment to multiple machines.
- Installation writes the selected tool root and command directory. Defaults are `/home/pypto-tools/pto-btop` and `/usr/local/bin`. The installer preserves `config/`, `state/`, `logs/`, `tmp/` and previous application releases; it does not start or restart services. Run it only within the user's authorized installation scope.
- Generate configuration explicitly with `app/current/scripts/https-init.py`. It refuses existing destinations. Preserve existing enrollment when upgrading. Each node receives only its own `nodes/<id>` bundle; the hub keeps the complete enrollment tree. Never display or commit tokens, keys or private configuration contents. Inspect field names and file permissions when diagnosing credentials.
- Enabling/restarting services, distributing credentials and changing NetBird policies affect running systems. Perform them only when the user authorized that action; installation alone does not authorize them. Use the generated user service templates in `app/systemd/`; ensure the chosen account owns its configuration and can read the required metrics.
- Always verify HTTPS CA and server names. Do not disable verification to fix connectivity. Listen on the selected NetBird or loopback IP. An agent needs outbound access to the hub; viewers do not need SSH.
- Preserve the source tree and current release when an installation or test fails. Do not publish a GitHub release merely because a package builds locally; remote publication must be in the user's scope, and both architecture jobs must succeed before calling both packages tested.

For all local login accounts, follow [system-service deployment](../../docs/cluster/system-service.md). The system installer reuses the original private installation, verifies the official stable release and its checksum, and publishes only viewer credentials. `--check` performs a read-only preflight; installation requires sudo; `--activate` explicitly migrates old processes and enables system services. The global launcher makes temporary private viewer copies to preserve the existing 0600 token check. Do not loosen agent or server credential permissions.

The one-command `install.sh` explicitly installs and activates shared services, auto-detecting the original account and hub/node role. Subsequent `sudo pto-btop-update [--proxy URL]` calls use private system deployment state, do not need the original home directory, select GitHub Latest stable releases, and preserve configuration. Already-current updates do not restart services; `--force` explicitly reinstalls them. Keep the bootstrap checksum synchronized whenever `scripts/system-install.py` changes.
