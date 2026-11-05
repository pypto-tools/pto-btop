# Contributing to pto-btop

This repository maintains the multi-machine terminal monitor. The original btop
contribution document is archived in [docs/upstream/CONTRIBUTING.md](docs/upstream/CONTRIBUTING.md);
it describes contributions to the upstream repository.

Describe the concrete behavior changed and the validation performed. Keep Linux
collection, the HTTPS protocol, and the terminal renderer independently testable.
Use C++23, tabs for C++ indentation, and `and`, `or`, `not` consistently with the
existing code. Preserve upstream copyright and license notices.

Use the build and test commands in [README.md](README.md). Changes to enrollment,
transport or installation should include checks of failure behavior, not just the
successful path. Do not commit private configuration, credentials, keys, real host
inventory or private screenshots. Use simulated data for public screenshots.

Public commands use `pto-` names. Installation and service activation are separate;
upgrades must preserve configuration and state. Update the relevant documentation
when a command, configuration field, runtime requirement or deployment behavior
changes. See [release procedure](docs/cluster/release.md) before making a version tag.
