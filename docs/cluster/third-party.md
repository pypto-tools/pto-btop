# Third-party notices

- **btop**: Apache-2.0, copyright aristocratos and contributors. This derivative
  starts at `e28a2701c7208ff00ab780a68287e2204f356ba0`; keep `LICENSE` and original
  file notices. Original repository: <https://github.com/aristocratos/btop>.
- **fmt**: MIT, license in `include/fmt/LICENSE.rst` (packaged as `fmt-LICENSE`).
- **nlohmann/json**: MIT, system build dependency >= 3.11; license in
  `include/nlohmann-LICENSE.MIT`. Local tested version is 3.11.3; Debian 12 CI
  uses its distribution package.
- **cpp-httplib**: MIT, fixed v0.64.0 header and license in `include/httplib/`;
  source URLs and digests are in its `upstream.json`.
- **GCC runtime (libstdc++ / libgcc)**: release builds statically link these
  libraries. Their applicable notices are GPLv3 with the GCC Runtime Library
  Exception 3.1; copies are in `include/gcc-runtime/COPYING3` and `COPYING.RUNTIME`.
  Compiler runtime source: <https://gcc.gnu.org/git.html>. This exception applies
  to eligible runtime code; it does not change this project's Apache-2.0 license.
- **OpenSSL 3** and **glibc**: runtime dependencies loaded from the target system;
  their shared libraries are not copied into these packages. OpenSSL is
  Apache-2.0; glibc uses LGPL-2.1-or-later and other notices in its distribution.

The binary package carries this file and the bundled-code license texts in
`share/btop-cluster/licenses/`. System runtime updates remain the responsibility
of the target distribution. The package does not contain enrolled credentials,
machine inventory, private certificates or private screenshots.
