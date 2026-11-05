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

for options in (["--low-color"], ["--theme", "TTY"]):
    result = subprocess.run([binary, "--demo", "--once", *options], capture_output=True, check=True, timeout=10)
    assert b"38;2;" not in result.stdout
    screen(result.stdout.decode(), 120, 36)

for options in ([], ["--demo", "--interval-ms", "1"], ["--demo", "--width", "-1"], ["--bad"]):
    result = subprocess.run([binary, *options], capture_output=True, timeout=10)
    assert result.returncode != 0
    assert b"\x1b[?1049h" not in result.stdout






for ending in ("q", signal.SIGINT, signal.SIGTERM):
    master, slave = os.openpty()
    resize(slave, 120, 36)
    original = termios.tcgetattr(slave)
    proc = subprocess.Popen([binary, "--demo"], stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    try:
        view = Screen(120, 36)
        view.wait_text(master, "Tab Select")
        assert not termios.tcgetattr(slave)[3] & termios.ICANON
        os.write(master, b"2")
        view.wait_text(master, "Esc Overview")
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
