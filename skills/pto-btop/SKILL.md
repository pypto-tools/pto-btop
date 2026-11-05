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
