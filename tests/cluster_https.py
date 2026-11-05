# SPDX-License-Identifier: Apache-2.0
"""Real loopback HTTPS hub, three agents and three independent terminal viewers."""
import copy
import http.client
import http.server
import threading
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import subprocess
import sys
import tempfile
import termios
import time

from terminal_support import Screen, resize

server_binary, agent_binary, client_binary, initializer = map(str, map(Path, sys.argv[1:5]))
children = []
ptys = []


def launch(command, **kwargs):
    process = subprocess.Popen(command, **kwargs)
    children.append(process)
    return process


def until(fn, timeout=12):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            last = fn()
            if last:
                return last
        except (ConnectionError, OSError, http.client.HTTPException) as error:
            last = str(error)
        time.sleep(0.1)
    raise AssertionError(f"Timed out: {last}")


def subscription(filter_text="", sort="cpu", selected=None):
    process = {"enabled": True, "sort": sort, "filter": filter_text, "limit": 500, "reverse": False}
    if selected:
        process.update(selected_pid=selected["pid"], selected_start=selected["start_time_ticks"])
    return {"type": "subscribe", "version": 1, "request_id": 1, "interval_ms": 1000, "detail": True,
            "processes": process, "mounts": [], "disk_devices": [], "network_interfaces": []}


with tempfile.TemporaryDirectory(prefix="btop-https-test-") as temporary:
    root = Path(temporary)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    bundle = root / "config"
    subprocess.run([sys.executable, initializer, "--directory", str(bundle), "--listen", "127.0.0.1",
                    "--server-name", "127.0.0.1", "--port", str(port), "--hosts", "a", "b", "c", "unit"],
                   check=True, capture_output=True, timeout=30)
    settings_path = bundle / "server/server.json"
    settings = json.loads(settings_path.read_text())
    for viewer in settings["viewers"]:
        viewer["hosts"] = ["a", "b", "c"] if viewer["id"] != "unit" else ["unit"]
    limited_token = secrets.token_hex(32)
    (bundle / "server/limited.token").write_text(limited_token)
    (bundle / "server/limited.token").chmod(0o600)
    settings["viewers"].append({"id": "limited", "hosts": ["a"], "token_file": "limited.token"})
    settings_path.write_text(json.dumps(settings))
    ctx = ssl.create_default_context(cafile=str(bundle / "ca.pem"))
    tokens = {(host, role): (bundle / f"nodes/{host}/{role}.token").read_text().strip()
              for host in ("a", "b", "c", "unit") for role in ("agent", "viewer")}

    def api(method, path, token, body=None, expected=200):
        connection = http.client.HTTPSConnection("127.0.0.1", port, context=ctx, timeout=5)
        try:
            payload = json.dumps(body) if body is not None else None
            connection.request(method, path, body=payload, headers={"Authorization": "Bearer " + token,
                                                                   "Content-Type": "application/json"})
            response = connection.getresponse()
            data = response.read()
            assert response.status == expected, (method, path, response.status, data[:200])
            return json.loads(data)
        finally:
            connection.close()

    def start_server():
        log = open(root / f"server-{len(children)}.log", "wb")
        process = launch([server_binary, "--config", str(settings_path)], stdout=log, stderr=log)
        try:
            until(lambda: api("GET", "/api/v1/hosts", tokens["a", "viewer"]), 8)
        except AssertionError as error:
            log.flush()
            raise AssertionError(f"{error}\nHub log: {Path(log.name).read_text()}") from error
        return process

    try:
        server = start_server()
        epoch = api("GET", "/api/v1/hosts", tokens["a", "viewer"])["epoch"]
        api("GET", "/api/v1/hosts", "wrong-token", expected=403)
        api("GET", "/api/v1/hosts", tokens["a", "agent"], expected=403)
        api("GET", "/api/v1/hosts/b/snapshot", limited_token, expected=403)
        api("POST", "/api/v1/agent/session", tokens["a", "viewer"], {}, expected=403)
        assert [host["id"] for host in api("GET", "/api/v1/hosts", limited_token)["hosts"]] == ["a"]

        # A separately authorized test host exercises atomic validation and session ordering.
        hello = {"version": 1, "type": "hello", "host": {"os": "linux", "arch": "fixture"}}
        registration = api("POST", "/api/v1/agent/session", tokens["unit", "agent"],
                           {"instance_id": "test-instance", "hello": hello})
        def group(data):
            return {"status": "ok", "data": data, "observed_monotonic_ms": 1, "interval_ms": 1000}
        body = {"session_id": registration["session_id"], "seq": 1, "interval_ms": 1000,
                "wall_time": "2026-11-05T00:00:00Z", "process_views": {}, "groups": {
                    "cpu": group({"usage_pct": 12, "online_cores": 1, "cores": []}),
                    "memory": group({"total_bytes": 4096, "used_bytes": 1024}),
                    "filesystems": group({"items": []}), "disks": group({"items": []}), "network": group({"items": []})}}
        api("POST", "/api/v1/agent/snapshots", tokens["unit", "agent"], body)
        api("POST", "/api/v1/agent/snapshots", tokens["unit", "agent"], body, expected=409)
        bad = copy.deepcopy(body)
        bad["seq"] = 2
        bad["groups"]["cpu"]["data"]["cores"] = [{"id": "bad"}]
        api("POST", "/api/v1/agent/snapshots", tokens["unit", "agent"], bad, expected=400)
        assert api("GET", "/api/v1/hosts/unit/snapshot", tokens["unit", "viewer"])["seq"] == 1
        body["seq"] = 2
        api("POST", "/api/v1/agent/snapshots", tokens["unit", "agent"], body)
        assert api("GET", "/api/v1/hosts/unit/history", tokens["unit", "viewer"])["history"]["history"] == [12, 12]
        denied = copy.deepcopy(body)
        denied["session_id"] = "old-session"
        denied["seq"] = 3
        api("POST", "/api/v1/agent/snapshots", tokens["unit", "agent"], denied, expected=409)
        assert set(registration["metric_groups"]) == {"cards", "sessions"}
        cards = json.loads((Path(initializer).parents[1] / "docs/cluster/examples/task-submit-monitor.example.json").read_text())
        body["seq"] = 3
        body["groups"]["cards"] = group(cards)
        body["groups"]["sessions"] = group({"source": "utmpx", "users": 1, "sessions": 2, "age_ms": 0,
                                           "items": [{"user": "alice", "sessions": 2, "first_login_at": 1000}]})
        api("POST", "/api/v1/agent/snapshots", tokens["unit", "agent"], body)
        stored = api("GET", "/api/v1/hosts/unit/snapshot", tokens["unit", "viewer"])["snapshot"]["groups"]
        assert stored["cards"]["data"]["devices"] == cards["devices"]
        for case in ("counts", "duplicate-login", "unknown-group"):
            bad = copy.deepcopy(body)
            bad["seq"] = 4
            if case == "counts":
                bad["groups"]["cards"]["data"]["summary"]["free"] += 1
            elif case == "duplicate-login":
                bad["groups"]["sessions"]["data"]["items"] *= 2
            else:
                bad["groups"]["unexpected"] = group({})
            api("POST", "/api/v1/agent/snapshots", tokens["unit", "agent"], bad, expected=400)
            assert api("GET", "/api/v1/hosts/unit/snapshot", tokens["unit", "viewer"])["seq"] == 3
        print("HTTPS authentication, host ACLs, atomic snapshots and sequence checks: PASS", flush=True)

        # Certificate trust and hostname verification must remain enabled in the C++ client.
        for case in ("wrong-ca", "wrong-host"):
            config = json.loads((bundle / "nodes/a/client.json").read_text())
            config["token_file"] = str(bundle / "nodes/a/viewer.token")
            config["ca_file"] = str(bundle / "ca.pem")
            if case == "wrong-host":
                config["server"] = f"https://localhost:{port}"
            else:
                subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                                "-keyout", str(root / "wrong.key"), "-out", str(root / "wrong.pem"),
                                "-subj", "/CN=wrong-ca"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                config["ca_file"] = str(root / "wrong.pem")
            path = root / f"{case}.json"
            path.write_text(json.dumps(config))
            result = subprocess.run([client_binary, "--config", str(path), "--check"], capture_output=True, timeout=8)
            assert result.returncode != 0 and b"SSL" in result.stderr, (case, result.stderr)
        print("Wrong CA and wrong server name rejected: PASS", flush=True)

        # An old hub does not advertise optional metric groups and rejects extras.
        legacy_snapshots = []
        class LegacyHub(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                code = 200
                if self.path.endswith("snapshots"):
                    legacy_snapshots.append(payload)
                    if set(payload["groups"]) != {"cpu", "memory", "filesystems", "disks", "network"}:
                        code = 400
                response = json.dumps({"session_id": "legacy-session", "requests": {}}).encode()
                self.send_response(code)
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

        legacy = http.server.HTTPServer(("127.0.0.1", 0), LegacyHub)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(bundle / "server/cert.pem", bundle / "server/key.pem")
        legacy.socket = tls.wrap_socket(legacy.socket, server_side=True)
        thread = threading.Thread(target=legacy.serve_forever, daemon=True)
        thread.start()
        config = json.loads((bundle / "nodes/a/agent.json").read_text())
        config.update(server=f"https://127.0.0.1:{legacy.server_port}", ca_file=str(bundle / "ca.pem"),
                      token_file=str(bundle / "nodes/a/agent.token"), interval_ms=1000)
        legacy_config = root / "legacy-agent.json"
        legacy_config.write_text(json.dumps(config))
        legacy_agent = launch([agent_binary, "--config", str(legacy_config)], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
        try:
            until(lambda: len(legacy_snapshots) >= 2, 10)
            assert legacy_agent.poll() is None
            assert all(set(item["groups"]) == {"cpu", "memory", "filesystems", "disks", "network"}
                       for item in legacy_snapshots)
        finally:
            legacy_agent.terminate()
            legacy_agent.wait(timeout=5)
            legacy.shutdown()
            legacy.server_close()
            thread.join(timeout=2)
        print("New agent / legacy hub feature negotiation: PASS", flush=True)

        agents = []
        for host in ("a", "b", "c"):
            path = bundle / f"nodes/{host}/agent.json"
            config = json.loads(path.read_text())
            config["interval_ms"] = 1000
            path.write_text(json.dumps(config))
            log = open(root / f"agent-{host}.log", "wb")
            agents.append(launch([agent_binary, "--config", str(path)], stdout=log, stderr=log))
        until(lambda: all(api("GET", f"/api/v1/hosts/{host}/snapshot", tokens["a", "viewer"])["seq"] >= 2
                          for host in ("a", "b", "c")), 12)

        for host in ("a", "b", "c"):
            groups = api("GET", f"/api/v1/hosts/{host}/snapshot", tokens["a", "viewer"])["snapshot"]["groups"]
            assert "cards" in groups and "sessions" in groups

        views = []
        for user, query in (("a", subscription("btop-agent", "rss")), ("b", subscription("no-such-process"))):
            view = api("POST", "/api/v1/views", tokens[user, "viewer"], {"host_id": "a", "subscription": query})
            views.append((user, view))
        def result_view(user, view):
            return api("GET", "/api/v1/hosts/a/snapshot?view=" + view["id"], tokens[user, "viewer"])["snapshot"]["groups"]["processes"]
        until(lambda: all(result_view(user, view)["status"] != "warming_up" for user, view in views), 10)
        first = result_view(*views[0])["data"]
        second = result_view(*views[1])["data"]
        assert first["items"] and all("btop-agent" in row["name"] for row in first["items"])
        assert second["items"] == []
        selected = first["items"][0]
        # User selectors are executed by the real agent, independently per viewer.
        owner = selected["user"]
        owner_part = owner[1:] if len(owner) > 1 else owner
        api("POST", "/api/v1/views", tokens["a", "viewer"], {"id": views[0][1]["id"], "host_id": "a",
             "subscription": subscription("user:" + owner_part.upper())})
        until(lambda: bool(result_view(*views[0])["data"].get("items")), 10)
        owned = result_view(*views[0])["data"]["items"]
        assert owned and all(owner_part.lower() in row["user"].lower() for row in owned)
        assert result_view(*views[1])["data"]["items"] == []
        api("POST", "/api/v1/views", tokens["a", "viewer"], {"id": views[0][1]["id"], "host_id": "a",
             "subscription": subscription("user:" + owner + "-no-such-account")})
        until(lambda: result_view(*views[0])["status"] != "warming_up", 10)
        assert result_view(*views[0])["data"]["items"] == []
        api("POST", "/api/v1/views", tokens["a", "viewer"], {"id": views[0][1]["id"], "host_id": "a",
             "subscription": subscription("user:")})
        until(lambda: result_view(*views[0])["status"] != "warming_up", 10)
        assert result_view(*views[0])["data"]["items"] == []
        api("POST", "/api/v1/views", tokens["a", "viewer"], {"id": views[0][1]["id"], "host_id": "a",
             "subscription": subscription("uid:" + str(selected["uid"]))})
        until(lambda: bool(result_view(*views[0])["data"].get("items")), 10)
        assert all(row["uid"] == selected["uid"] for row in result_view(*views[0])["data"]["items"])
        assert result_view(*views[1])["data"]["items"] == []
        api("POST", "/api/v1/views", tokens["b", "viewer"], {"id": views[1][1]["id"], "host_id": "a",
             "subscription": subscription("no-such-process", selected=selected)})
        until(lambda: (result_view(*views[1])["data"].get("selected") or {}).get("pid") == selected["pid"], 10)
        api("GET", "/api/v1/hosts/a/snapshot?view=" + views[1][1]["id"], tokens["a", "viewer"], expected=403)
        api("DELETE", "/api/v1/views/" + views[1][1]["id"], tokens["a", "viewer"], expected=403)
        for user, view in views:
            api("DELETE", "/api/v1/views/" + view["id"], tokens[user, "viewer"])
        print("Independent name/user filters, sorts, selected PID outside filter, view ownership: PASS", flush=True)

        # Terminal read paths operate concurrently and remain responsive during hub failure.
        for index, host in enumerate(("a", "b", "c")):
            master, slave = os.openpty()
            resize(slave, 120, 36)
            original = termios.tcgetattr(slave)
            process = launch([client_binary, "--config", str(bundle / f"nodes/{host}/client.json"), "--detail"],
                             stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
            screen = Screen(120, 36)
            screen.wait_text(master, "Esc Overview", timeout=10)
            os.write(master, str(index + 1).encode())
            screen.wait_text(master, "ONLINE", timeout=10)
            ptys.append((master, slave, process, screen, original))
        os.write(ptys[0][0], b"m/btop-agent\r")
        ptys[0][3].wait_text(ptys[0][0], "/btop-agent", timeout=10)
        os.write(ptys[1][0], b"T")
        ptys[1][3].wait_text(ptys[1][0], "TREE", timeout=10)
        assert "TREE" not in ptys[2][3].text()
        check = subprocess.run([client_binary, "--config", str(bundle / "nodes/c/client.json"), "--check"],
                               capture_output=True, text=True, timeout=18)
        assert check.returncode == 0 and check.stdout.count("ONLINE") == 3, check.stdout + check.stderr
        print("Three real agents, three independent PTYs and --check: PASS", flush=True)

        agents[2].terminate()
        assert agents[2].wait(timeout=5) == 0
        until(lambda: api("GET", "/api/v1/hosts/c/snapshot", tokens["a", "viewer"])["status"] == "DISCONNECTED", 13)
        assert api("GET", "/api/v1/hosts/a/snapshot", tokens["a", "viewer"])["status"] == "ONLINE"
        expired = api("POST", "/api/v1/views", tokens["a", "viewer"], {"host_id": "a", "subscription": subscription()})
        time.sleep(15.3)
        api("GET", "/api/v1/hosts/a/snapshot?view=" + expired["id"], tokens["a", "viewer"], expected=404)
        server.terminate()
        assert server.wait(timeout=6) == 0
        ptys[0][3].wait_text(ptys[0][0], "DISCONNECTED", timeout=15)
        started = time.monotonic()
        for master, slave, process, screen, original in ptys[1:]:
            os.write(master, b"q")
        for master, slave, process, screen, original in ptys[1:]:
            assert process.wait(timeout=5) == 0
            assert termios.tcgetattr(slave) == original
        assert time.monotonic() - started < 5
        server = start_server()
        assert api("GET", "/api/v1/hosts", tokens["a", "viewer"])["epoch"] != epoch
        until(lambda: api("GET", "/api/v1/hosts/a/snapshot", tokens["a", "viewer"])["seq"] >= 2, 15)
        ptys[0][3].wait_text(ptys[0][0], "ONLINE", timeout=15)
        os.write(ptys[0][0], b"q")
        assert ptys[0][2].wait(timeout=5) == 0
        assert termios.tcgetattr(ptys[0][1]) == ptys[0][4]
        print("Stale/offline isolation, lease expiry, responsive exit, live viewer and agent restart recovery: PASS", flush=True)
    finally:
        for process in reversed(children):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for master, slave, *_ in ptys:
            os.close(master)
            os.close(slave)
