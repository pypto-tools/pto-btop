#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run inside the pinned manylinux image. All outputs stay under the supplied directory.
set -euo pipefail

prefix=$(realpath -m "${1:?Usage: build-release-deps.sh PREFIX}")
jobs=${PTO_BUILD_JOBS:-2}
mkdir -p "$prefix/sources"
cd "$prefix/sources"

fetch() {
  local name=$1 digest=$2 url=$3
  if [ ! -f "$name" ]; then
    curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' \
      --connect-timeout 20 --max-time 300 --output "$name" "$url"
  fi
  printf '%s  %s\n' "$digest" "$name" | sha256sum --check
}

fetch openssl-3.5.9.tar.gz 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a \
  https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
tar -xzf openssl-3.5.9.tar.gz
(
  cd openssl-3.5.9
  ./Configure --prefix="$prefix" --libdir=lib --openssldir=/etc/ssl \
    no-shared no-module no-tests -fPIC
  make -j "$jobs"
  make install_sw
)

fetch json.tar.xz d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d \
  https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
tar -xJf json.tar.xz
cmake -S json -B json-build -DJSON_BuildTests=OFF -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --install json-build
