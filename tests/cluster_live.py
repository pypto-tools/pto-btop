# SPDX-License-Identifier: Apache-2.0
"""Headless protocol, local live UI, and four SSH sessions through a fake transport."""
import json
import os
import select
import signal
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

from terminal_support import Screen, resize, screen

agent, client = map(os.path.abspath, sys.argv[1:3])


class Reader:
    def __init__(self, stream):
        self.fd = stream.fileno()
        self.pending = b""

    def get(self, timeout=5):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "Protocol timeout"
            assert select.select([self.fd], [], [], remaining)[0]
            chunk = os.read(self.fd, 65536)
            assert chunk, "Unexpected EOF"
            self.pending += chunk
        line, self.pending = self.pending.split(b"\n", 1)
        return json.loads(line)


def request(identity, detail=True, filter_text="", sort="cpu"):
    return {"type": "subscribe", "version": 1, "request_id": identity,
            "interval_ms": 1000, "detail": detail,
            "processes": {"enabled": detail, "sort": sort, "filter": filter_text, "limit": 100},
            "mounts": [], "disk_devices": [], "network_interfaces": []}


def send(process, message):
    process.stdin.write(json.dumps(message).encode() + b"\n")
    process.stdin.flush()


process = subprocess.Popen([agent, "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    reader = Reader(process.stdout)
    hello = reader.get()
    assert hello["type"] == "hello"
    assert set(hello["capabilities"]) == {"cpu", "memory", "filesystems", "disks", "network", "processes", "sessions", "cards", "card_usage", "uid_filter"}
    send(process, request(1))
    assert reader.get()["type"] == "subscribed"
    first = reader.get()
    second = reader.get()
    assert first["groups"]["cpu"]["data"]["usage_pct"] is None
    assert 0 <= second["groups"]["cpu"]["data"]["usage_pct"] <= 100
    assert second["seq"] > first["seq"]
    assert set(second["groups"]) == set(hello["capabilities"]) - {"uid_filter"}
    mem = second["groups"]["memory"]["data"]
    assert mem["used_bytes"] == mem["total_bytes"] - mem["available_bytes"]
    assert b"\x1b" not in json.dumps(second).encode()
    for group in second["groups"].values():
        assert group["observed_monotonic_ms"] <= second["monotonic_ms"]
    send(process, request(2, filter_text="btop-agent", sort="rss"))
    while reader.get()["type"] != "subscribed":
        pass
    filtered = reader.get()
    assert filtered["subscription_id"] == 2
    for item in filtered["groups"]["processes"]["data"]["items"]:
        assert "btop-agent" in item["name"].lower()
    candidates = filtered["groups"]["processes"]["data"]["items"]
    assert candidates
    selected_request = request(3, filter_text="does-not-match", sort="threads")
    selected_request["processes"].update(selected_pid=candidates[0]["pid"], selected_start=candidates[0]["start_time_ticks"])
    send(process, selected_request)
    while reader.get()["type"] != "subscribed":
        pass
    selection = reader.get()["groups"]["processes"]["data"]
    assert selection["items"] == []
    assert selection["selected"]["detail_status"] == "ok"
    assert selection["selected"]["pid"] == candidates[0]["pid"]
    assert selection["selected"]["command"] and selection["selected"]["user"]
    send(process, request(4, detail=False))
    while reader.get()["type"] != "subscribed":
        pass
    summary = reader.get()
    assert "processes" not in summary["groups"]
    assert summary["groups"]["cpu"]["data"]["cores"] == []
    send(process, {"type": "stop", "version": 1, "request_id": 5})
    assert process.wait(timeout=5) == 0
finally:
    if process.poll() is None:
        process.kill()
        process.wait()

for malformed in (
    b'{"version":1,"version":1,"type":"subscribe"}\n',
    b'{"version":99,"type":"subscribe"}\n',
):
    result = subprocess.run([agent, "--stdio"], input=malformed, capture_output=True, timeout=5)
    assert result.returncode != 0
    messages = [json.loads(line) for line in result.stdout.splitlines()]
    assert messages[-1]["type"] == "error" and messages[-1]["fatal"]

for width, height in ((80, 24), (100, 30), (120, 36), (160, 48), (200, 60)):
    result = subprocess.run([client, "--demo", "--detail", "--once", "--width", str(width), "--height", str(height)],
                            capture_output=True, text=True, check=True, timeout=10)
    rendered = screen(result.stdout, width, height)
    for title in ("cpu ", "mem", "disks", "net", "proc"):
        assert title in rendered, (width, height, title)

result = subprocess.run([client, "--local", "--detail", "--once", "--interval-ms", "1000"],
                        capture_output=True, text=True, check=True, timeout=15)
assert "LIVE" in result.stdout and "SIMULATED" not in result.stdout
screen(result.stdout, 120, 36)

master, slave = os.openpty()
resize(slave, 120, 36)
original = termios.tcgetattr(slave)
process = subprocess.Popen([client, "--local", "--detail", "--interval-ms", "1000"],
                           stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
try:
    view = Screen(120, 36)
    view.wait_text(master, "ONLINE", timeout=10)
    os.write(master, b"m")
    view.wait_text(master, "proc | rss")
    os.write(master, b"/btop-agent\r")
    view.wait_text(master, "/btop-agent")
    os.write(master, b"]ndj")
    view.wait_text(master, "proc | rss")
    resize(slave, 80, 24)
    assert "Esc Overview" in view.resize_when_cleared(master, 80, 24)
    process.send_signal(signal.SIGTERM)
    assert process.wait(timeout=5) == 0
    assert termios.tcgetattr(slave) == original
finally:
    if process.poll() is None:
        process.kill()
        process.wait()
    os.close(master)
    os.close(slave)

# Exercise actual expanded views, not just their initial frame, at each terminal size.
for width, height in ((80, 24), (100, 30), (120, 36), (160, 48), (200, 60)):
    master, slave = os.openpty()
    resize(slave, width, height)
    process = subprocess.Popen([client, "--demo", "--detail"], stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    try:
        view = Screen(width, height)
        view.wait_text(master, "Esc Overview")
        os.write(master, b"\x1b[B")
        view.wait_text(master, "Esc Overview")
        os.write(master, b"\r")
        assert "proc info" in view.wait_text(master, "proc info")
        for key, marker in ((b"T", b"TREE"), (b"i", b"I/O"), (b"g", b"user_pct"), (b"?", b"Keys")):
            os.write(master, key)
            view.wait_text(master, marker.decode())
        os.write(master, b"?")
        view.wait_text(master, "Esc Overview")
        os.write(master, b"q")
        assert process.wait(timeout=5) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)

# Substitute ssh itself, preserving the exact argv contract; no network or credentials required.
with tempfile.TemporaryDirectory(prefix="btop-fake-ssh-") as temporary:
    wrapper = Path(temporary, "ssh")
    wrapper.write_text("#!/usr/bin/env python3\nimport os,sys\n"
                       "args=sys.argv[1:]\n"
                       "if os.environ.get('BTOP_TEST_FAIL_HOST')==args[-3]:\n"
                       "    sys.stderr.write('simulated SSH connection failure\\n'); sys.exit(1)\n"
                       "assert args[0]=='-T' and 'BatchMode=yes' in args\n"
                       "assert 'StrictHostKeyChecking=yes' in args\n"
                       "assert args[-1]=='--stdio'\n"
                       "os.execv(args[-2],[args[-2],args[-1]])\n")
    wrapper.chmod(0o755)
    environment = dict(os.environ, PATH=temporary + os.pathsep + os.environ["PATH"])
    result = subprocess.run([client, "--ssh", "dev-01", "--ssh", "dev-02", "--ssh", "dev-03", "--ssh", "dev-04",
                             "--agent-path", agent, "--once", "--interval-ms", "1000"],
                            env=environment, capture_output=True, text=True, check=True, timeout=20)
    rendered = screen(result.stdout, 120, 36)
    for index in range(1, 5):
        assert f"dev-{index:02}" in rendered
    assert "SIMULATED" not in rendered

    environment["BTOP_TEST_FAIL_HOST"] = "dev-04"
    failed = subprocess.run([client, "--ssh", "dev-01", "--ssh", "dev-02", "--ssh", "dev-03", "--ssh", "dev-04",
                             "--agent-path", agent, "--once", "--interval-ms", "1000"],
                            env=environment, capture_output=True, text=True, timeout=20)
    assert failed.returncode == 1
    rendered = screen(failed.stdout, 120, 36)
    assert rendered.count("ONLINE") >= 3
    assert "DISCONNECTED" in rendered or "CONNECTING" in rendered

for alias in ("-bad", "host;touch /tmp/should-not-exist", "user@host"):
    result = subprocess.run([client, "--ssh", alias, "--once"], capture_output=True, timeout=5)
    assert result.returncode != 0 and b"\x1b[?1049h" not in result.stdout

print("Protocol lifecycle, all live groups, filter/sort, detail geometry, local PTY and four SSH transports: PASS")
