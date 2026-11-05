---
name: Bug report
about: Report a pto-btop monitoring, connection, installation or display problem
labels: bug
---

## Problem and reproduction

Describe what happened, what you expected, and the steps to reproduce it.
State whether it occurs in HTTPS, local, SSH or demo mode.

## Environment

- `pto-btop --version`, `pto-btop-agent --version`, `pto-btop-server --version`
- Release package or source build
- Linux distribution, `uname -m`, `getconf GNU_LIBC_VERSION`
- Terminal application and dimensions (for display problems)
- Whether `pto-btop --local --check` works
- Whether the hub can be reached from the affected machine

## Evidence

Include relevant error messages or a screenshot using `--demo` where possible.
Remove host addresses, usernames and private command arguments. Do not include
tokens, private keys or a complete enrollment/configuration directory.
