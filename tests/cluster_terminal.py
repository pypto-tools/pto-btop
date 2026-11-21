# SPDX-License-Identifier: Apache-2.0
"""Exercise the prototype through a PTY and check real ANSI frame geometry."""

import os
import signal
import subprocess
import sys
import tempfile
import termios
import time


from terminal_support import Screen, screen, resize, read_until


binary = sys.argv[1]




for width, height in ((120, 36), (160, 48), (80, 24), (119, 36), (40, 10), (1, 1)):
    result = subprocess.run(
        [binary, "--demo", "--once", "--width", str(width), "--height", str(height)],
        capture_output=True, text=True, check=True, timeout=10,
    )
    rendered = screen(result.stdout, width, height)
    if width >= 80 and height >= 24:
        assert "DEMO - simulated data" in rendered
        for host in range(1, 5):
            assert f"dev-{host:02}" in rendered
    elif width > 35:
        assert "Resize terminal" in rendered

# A spare grid cell shows a summary and has no host shortcut.
overview_cases = [(3, width, height, theme)
                  for width, height in ((120, 36), (160, 48), (200, 60), (121, 37), (80, 24))
                  for theme in ("Default", "TTY")]
overview_cases.extend(((5, 120, 36, "Default"), (5, 121, 37, "TTY"), (5, 200, 60, "Default"),
                       (5, 80, 24, "TTY"), (6, 120, 36, "TTY"), (6, 80, 24, "Default")))
for count, width, height, theme in overview_cases:
    result = subprocess.run([binary, "--demo", "--demo-hosts", str(count), "--once",
                             "--width", str(width), "--height", str(height), "--theme", theme],
                            capture_output=True, text=True, check=True, timeout=10)
    rendered = screen(result.stdout, width, height)
    visible = min(count, 4)
    summary = width >= 120 and height >= 36 and visible == 3
    assert ("cluster | overview" in rendered) == summary
    assert f"dev-{visible + 1:02}" not in rendered
    assert f"1-{visible} Host" in rendered
    if count > 4:
        assert f"Page 1/2  Hosts 1-4/{count}" in rendered
    for host in range(1, visible + 1):
        assert rendered.count(f"dev-{host:02}") == (2 if summary else 1)

for options in (["--low-color"], ["--theme", "TTY"]):
    result = subprocess.run([binary, "--demo", "--once", *options], capture_output=True, check=True, timeout=10)
    assert b"38;2;" not in result.stdout
    screen(result.stdout.decode(), 120, 36)

for options in ([], ["--demo", "--interval-ms", "1"], ["--demo", "--width", "-1"], ["--bad"],
                ["--demo", "--demo-hosts", "0"], ["--demo", "--demo-hosts", "-1"],
                ["--local", "--demo-hosts", "3"]):
    result = subprocess.run([binary, *options], capture_output=True, timeout=10)
    assert result.returncode != 0
    assert b"\x1b[?1049h" not in result.stdout






for ending in ("q", signal.SIGINT, signal.SIGTERM):
    master, slave = os.openpty()
    resize(slave, 120, 36)
    original = termios.tcgetattr(slave)
    proc = subprocess.Popen([binary, "--demo", "--demo-hosts", "5"], stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    try:
        view = Screen(120, 36)
        view.wait_text(master, "Tab Select")
        assert not termios.tcgetattr(slave)[3] & termios.ICANON
        os.write(master, b"\x1b[6~")
        last = view.wait_text(master, "Page 2/2  Hosts 5-5/5")
        assert "dev-05" in last and "dev-01" not in last
        os.write(master, b"1")
        assert "Esc Overview" in view.wait_text(master, "cpu dev-05")
        os.write(master, b"\t")
        view.wait_text(master, "cpu dev-01")
        os.write(master, b"\x1b")
        view.wait_text(master, "Tab Select")
        resize(slave, 80, 24)
        assert "Compact overview" in view.resize_when_cleared(master, 80, 24)
        os.write(master, b"t")
        view.wait_text(master, "Compact overview")
        if ending == "q":
            os.write(master, b"q")
        else:
            proc.send_signal(ending)
        proc.wait(timeout=5)
        assert proc.returncode == 0
        read_until(master, b"\x1b[?25h")
        assert termios.tcgetattr(slave) == original, "Terminal settings were not restored"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)

# Page-local shortcuts and direct jumps work beyond single-digit host numbers.
master, slave = os.openpty()
resize(slave, 120, 36)
proc = subprocess.Popen([binary, "--demo", "--demo-hosts", "23"], stdin=slave, stdout=slave,
                        stderr=slave, start_new_session=True)
try:
    view = Screen(120, 36)
    view.wait_text(master, "Page 1/6")
    os.write(master, b"#21\r")
    view.wait_text(master, "cpu dev-21")
    os.write(master, b"\x1b")
    last = view.wait_text(master, "Page 6/6  Hosts 21-23/23")
    assert "cluster | overview" in last and "23/23" in last and "dev-20" not in last
    os.write(master, b"\x1b[Z")
    view.wait_text(master, "Page 5/6")
    resize(slave, 80, 24)
    assert "Hosts 17-20/23" in view.resize_when_cleared(master, 80, 24)
    os.write(master, b"\x1b[5~")
    view.wait_text(master, "Hosts 13-16/23")
    os.write(master, b"4")
    view.wait_text(master, "cpu dev-16")
    os.write(master, b"\x1b")
    view.wait_text(master, "Page 4/6")
    os.write(master, b"\x1b[H")
    view.wait_text(master, "Page 1/6")
    os.write(master, b"\x1b[F")
    view.wait_text(master, "Page 6/6")
    os.write(master, b"h")
    usage = view.wait_text(master, "card hours | all servers")
    assert "DATA 23/23" in usage and "card hours | Hosts 21-23/23" in usage and "dev-23" in usage
    os.write(master, b"q")
    assert proc.wait(timeout=5) == 0
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(master)
    os.close(slave)

# Card focus, exact owner filter, login overlay and narrow-terminal paging.
master, slave = os.openpty()
resize(slave, 160, 48)
proc = subprocess.Popen([binary, "--demo", "--detail"], stdin=slave, stdout=slave,
                        stderr=slave, start_new_session=True)
try:
    view = Screen(160, 48)
    view.wait_text(master, "cards | queue")
    os.write(master, b"u")
    view.wait_text(master, "SINCE (UTC)")
    os.write(master, b"uvjj\r")
    view.wait_text(master, "Task: task-")
    os.write(master, b"f")
    view.wait_text(master, "/uid:1000")
    os.write(master, b"v")
    view.wait_text(master, "cards [focus]")
    os.write(master, b"\x1b[H")
    view.wait_text(master, "1/8")
    resize(slave, 120, 36)
    assert "1/8 p1/3" in view.resize_when_cleared(master, 120, 36)
    # Expanded details leave three visible cards. A page must move by three,
    # rather than jumping eight cards and skipping the middle page.
    for key, position in ((b"\x1b[6~", "4/8 p2/3"), (b"\x1b[6~", "7/8 p3/3"),
                          (b"\x1b[5~", "4/8 p2/3")):
        os.write(master, key)
        view.wait_text(master, position)
    os.write(master, b"v")
    view.wait_text(master, "cards | queue")
    os.write(master, b".")
    rendered = view.wait_text(master, "7/8 p3/3")
    assert "cards [focus]" in rendered
    os.write(master, b",")
    view.wait_text(master, "4/8 p2/3")
    resize(slave, 80, 24)
    assert "cards [focus]" in view.resize_when_cleared(master, 80, 24)
    os.write(master, b"\x1b[F")
    view.wait_text(master, "8/8")
    os.write(master, b"o")
    view.wait_text(master, "proc")
    os.write(master, b"q")
    assert proc.wait(timeout=5) == 0
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(master)
    os.close(slave)

# Usage overlays, periods, fuzzy user filters and the CARDS usage tab.
master, slave = os.openpty()
resize(slave, 80, 24)
proc = subprocess.Popen([binary, "--demo"], stdin=slave, stdout=slave,
                        stderr=slave, start_new_session=True)
try:
    view = Screen(80, 24)
    view.wait_text(master, "Compact overview")
    os.write(master, b"h")
    view.wait_text(master, "card hours | all servers")
    os.write(master, b"\x1b[6~")
    view.wait_text(master, "10/12")
    os.write(master, b"p")
    view.wait_text(master, "7d")
    os.write(master, b"/ALICE\r")
    rendered = view.wait_text(master, "Filter: ALICE")
    assert "ci-runner" not in rendered
    os.write(master, b"\x1b")
    view.wait_text(master, "Compact overview")
    os.write(master, b"1H")
    view.wait_text(master, "devices / USAGE")
    os.write(master, b"\r")
    view.wait_text(master, "card hours | dev-01")
    resize(slave, 120, 36)
    assert "card hours | dev-01" in view.resize_when_cleared(master, 120, 36)
    os.write(master, b"h")
    view.wait_text(master, "devices / USAGE")
    os.write(master, b"Hq")
    assert proc.wait(timeout=5) == 0
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(master)
    os.close(slave)

if len(sys.argv) > 2:
    with tempfile.TemporaryDirectory(prefix="btop-terminal-") as temporary:
        config = os.path.join(temporary, "btop.conf")
        with open(config, "w", encoding="utf-8") as output:
            output.write('shown_boxes = "cpu mem"\ncheck_temp = False\nupdate_ms = 1000\n')
        master, slave = os.openpty()
        resize(slave, 120, 36)
        original = termios.tcgetattr(slave)
        proc = subprocess.Popen(
            [sys.argv[2], "--config", config], stdin=slave, stdout=slave,
            stderr=slave, start_new_session=True,
        )
        try:
            output = read_until(master, b"cpu", timeout=15)
            assert b"\x1b[?1049h" in output
            os.write(master, b"q")
            proc.wait(timeout=10)
            assert proc.returncode == 0
            assert termios.tcgetattr(slave) == original
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)


# Periodic sampling must not expose clear/blank/redraw cycles to the terminal.
master, slave = os.openpty()
resize(slave, 120, 36)
original = termios.tcgetattr(slave)
proc = subprocess.Popen([binary, "--demo", "--detail", "--interval-ms", "1000"],
                        stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
try:
    view = Screen(120, 36)
    view.wait_text(master, "Esc Overview")
    first_size = len(view.last_frame)
    update_sizes = []
    for _ in range(3):
        view.read_frame(master)
        assert b"\x1b[2J" not in view.last_frame, "Periodic redraw cleared the screen"
        assert b"btop-cluster" not in view.last_frame, "Unchanged heading was repainted"
        assert "btop-cluster" in view.text() and "Esc Overview" in view.text()
        update_sizes.append(len(view.last_frame))
    assert max(update_sizes) < first_size, (first_size, update_sizes)
    print("Differential frame bytes:", first_size, "initial;", update_sizes, "updates")
    # Leave output unread, then interrupt while a synchronized frame may be partial.
    time.sleep(2.2)
    proc.send_signal(signal.SIGTERM)
    assert proc.wait(timeout=3) == 0
    assert termios.tcgetattr(slave) == original
    ending = read_until(master, b"\x1b[?25h")
    assert ending.rfind(b"\x1b[?2026l") > ending.rfind(b"\x1b[?2026h")
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(master)
    os.close(slave)

print("ANSI geometry, differential refresh, input, resize, color modes, legacy startup and terminal restoration: PASS")
