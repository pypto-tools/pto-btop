#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Create a private CA, hub config and one credential bundle per Linux host."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True, help="New private directory; must not already exist")
    parser.add_argument("--listen", required=True, help="Specific NetBird or loopback IP to bind")
    parser.add_argument("--server-name", required=True, help="DNS name or IP used in the HTTPS URL")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--hosts", nargs="+", required=True, help="One to four stable host IDs")
    args = parser.parse_args()
    address = ipaddress.ip_address(args.listen)
    if address.is_unspecified or not 1 <= args.port <= 65535:
        parser.error("Use a specific IP and a port from 1 through 65535")
    if not 1 <= len(args.hosts) <= 4 or len(set(args.hosts)) != len(args.hosts):
        parser.error("Use one to four distinct host IDs")
    if not all(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", host) for host in args.hosts):
        parser.error("Invalid host ID")
    try:
        name_ip = ipaddress.ip_address(args.server_name)
        san = f"IP:{name_ip}"
        url_name = f"[{name_ip}]" if name_ip.version == 6 else str(name_ip)
    except ValueError:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]{0,252}", args.server_name):
            parser.error("Invalid DNS name")
        san = "DNS:" + args.server_name
        url_name = args.server_name
    if args.directory.exists():
        parser.error("Directory already exists; refusing to overwrite keys or configuration")
    if not shutil.which("openssl"):
        parser.error("The openssl command is required")
    os.umask(0o077)
    root = args.directory.absolute()
    root.mkdir(parents=True, mode=0o700)
    server = root / "server"
    server.mkdir()

    def run(*command):
        subprocess.run(command, cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def write_json(path, data):
        path.write_text(json.dumps(data, indent=2) + "\n")

    run("openssl", "req", "-x509", "-newkey", "rsa:3072", "-nodes", "-days", "3650",
        "-keyout", "ca-key.pem", "-out", "ca.pem", "-subj", "/CN=btop-cluster private CA",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    run("openssl", "req", "-newkey", "rsa:3072", "-nodes", "-keyout", "server/key.pem",
        "-out", "server/request.pem", "-subj", "/CN=btop-cluster server")
    (server / "extensions.cnf").write_text(
        "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth\nsubjectAltName=" + san + "\n")
    run("openssl", "x509", "-req", "-in", "server/request.pem", "-CA", "ca.pem", "-CAkey", "ca-key.pem",
        "-CAcreateserial", "-days", "365", "-out", "server/cert.pem", "-extfile", "server/extensions.cnf")
    hosts, viewers = [], []
    for host in args.hosts:
        node = root / "nodes" / host
        node.mkdir(parents=True)
        for role in ("agent", "viewer"):
            (node / f"{role}.token").write_text(secrets.token_hex(32) + "\n")
        shutil.copyfile(root / "ca.pem", node / "ca.pem")
        shared = {"version": 1, "server": f"https://{url_name}:{args.port}", "ca_file": "ca.pem"}
        write_json(node / "agent.json", dict(shared, token_file="agent.token", interval_ms=2000))
        write_json(node / "client.json", dict(shared, token_file="viewer.token"))
        hosts.append({"id": host, "label": host, "token_file": f"../nodes/{host}/agent.token"})
        viewers.append({"id": host, "hosts": args.hosts, "token_file": f"../nodes/{host}/viewer.token"})
    write_json(server / "server.json", {"version": 1, "listen": args.listen, "port": args.port,
        "cert_file": "cert.pem", "key_file": "key.pem", "hosts": hosts, "viewers": viewers})
    print(f"Hub configuration: {server / 'server.json'}")
    print(f"Per-host bundles: {root / 'nodes'}")
    print("Keep the CA private key and server directory on the hub; copy only each host's own node bundle.")


if __name__ == "__main__":
    main()
