#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Install/update shared terminal monitoring from the existing local enrollment.
set -euo pipefail

installer_sha256='1ac0163fdefe0887a4fc96c265781f68b9897dee37f7efd7f5a423bab10c47c5'
installer_url='https://raw.githubusercontent.com/pypto-tools/pto-btop/main/scripts/system-install.py'
arguments=("$@")
proxy="${HTTPS_PROXY:-${https_proxy:-}}"
explicit_proxy=false
activate=(--activate --force)
for ((i=0; i<${#arguments[@]}; i++)); do
    case "${arguments[i]}" in
        --proxy)
            if (( i + 1 >= ${#arguments[@]} )); then
                echo '--proxy requires a URL' >&2
                exit 2
            fi
            proxy="${arguments[i+1]}"
            explicit_proxy=true
            ;;
        --proxy=*) proxy="${arguments[i]#--proxy=}"; explicit_proxy=true ;;
        --check|--help) activate=() ;;
    esac
done
route=()
if [[ -n "$proxy" ]]; then
    route=(--proxy "$proxy" --noproxy '')
    if [[ "$explicit_proxy" == false ]]; then
        arguments+=(--proxy "$proxy")
    fi
fi

temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
curl --disable --fail --silent --show-error --location \
    --proto '=https' --proto-redir '=https' --connect-timeout 15 --max-time 60 \
    --max-filesize 1048576 "${route[@]}" \
    --output "$temporary/system-install.py" "$installer_url"
if ! printf '%s  %s\n' "$installer_sha256" "$temporary/system-install.py" | sha256sum --check --status; then
    echo 'Installer checksum mismatch; nothing installed. Retry to fetch a consistent version.' >&2
    exit 1
fi

if [[ $(id -u) == 0 ]]; then
    /usr/bin/python3 "$temporary/system-install.py" "${activate[@]}" "${arguments[@]}"
else
    sudo /usr/bin/python3 "$temporary/system-install.py" "${activate[@]}" "${arguments[@]}"
fi

# Retarget only the invoking user's known legacy link, without changing any
# credentials or unrelated commands. This runs as that user, never as root.
if (( ${#activate[@]} > 0 )) && [[ $(id -u) != 0 && -L "$HOME/.local/bin/pto-btop" ]]; then
    if [[ $(readlink "$HOME/.local/bin/pto-btop") == "$HOME/.local/share/pto-btop/app/current/bin/pto-btop" ]]; then
        ln -sfn /usr/local/bin/pto-btop "$HOME/.local/bin/pto-btop"
    fi
fi
