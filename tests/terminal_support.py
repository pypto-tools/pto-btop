# SPDX-License-Identifier: Apache-2.0
import fcntl
import os
import re
import select
import struct
import termios
import time
import unicodedata

class Screen:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.cells = [[" " for _ in range(width)] for _ in range(height)]
        self.row = self.col = 0
        self.pending = b""
        self.last_frame = b""
        self.awaiting_size = None

    def feed(self, frame):
        index = 0
        while index < len(frame):
            if frame[index] == "\x1b":
                match = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", frame[index:])
                assert match, repr(frame[index:index + 20])
                raw, code = match.groups()
                args = [int(n) if n else 0 for n in raw.split(";")] if "?" not in raw else []
                if code in ("H", "f"):
                    self.row, self.col = max(1, args[0]) - 1, max(1, args[1]) - 1
                elif code == "C":
                    self.col += args[0]
                elif code == "D":
                    self.col -= args[0]
                elif code == "A":
                    self.row -= args[0]
                elif code == "B":
                    self.row += args[0]
                elif code == "J" and args == [2]:
                    self.cells = [[" " for _ in range(self.width)] for _ in range(self.height)]
                index += len(match[0])
                continue
            char = frame[index]
            index += 1
            if char == "\n":
                self.row += 1
                continue
            if char == "\r":
                self.col = 0
                continue
            if unicodedata.combining(char):
                continue
            span = 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1
            assert 0 <= self.row < self.height and 0 <= self.col and self.col + span <= self.width, (char, self.row, self.col, self.width, self.height)
            self.cells[self.row][self.col] = char
            if span == 2:
                self.cells[self.row][self.col + 1] = ""
            self.col += span
        return self.text()

    def text(self):
        return "\n".join("".join(line).rstrip() for line in self.cells)

    def read_frame(self, master, timeout=5):
        deadline = time.monotonic() + timeout
        end = b"\x1b[?2026l"
        while end not in self.pending:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "No completed terminal frame"
            assert select.select([master], [], [], remaining)[0], "No terminal output"
            self.pending += os.read(master, 65536)
        frame, self.pending = self.pending.split(end, 1)
        self.last_frame = frame + end
        if self.awaiting_size and b"\x1b[2J" in frame:
            self.width, self.height = self.awaiting_size
            self.awaiting_size = None
            self.cells = [[" " for _ in range(self.width)] for _ in range(self.height)]
            self.row = self.col = 0
        return self.feed(self.last_frame.decode())

    def resize_when_cleared(self, master, width, height, timeout=5):
        # Already-written frames still have the old geometry. Keep their buffered bytes
        # until the client's first full repaint acknowledges the new dimensions.
        self.awaiting_size = (width, height)
        deadline = time.monotonic() + timeout
        while self.awaiting_size:
            self.read_frame(master, max(0.01, deadline - time.monotonic()))
            assert time.monotonic() < deadline
        return self.text()

    def wait_text(self, master, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            rendered = self.read_frame(master, max(0.01, deadline - time.monotonic()))
            if marker in rendered:
                return rendered
            assert time.monotonic() < deadline, (marker, rendered)


def screen(frame, width, height):
    return Screen(width, height).feed(frame)

def resize(slave, width, height):
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))

def read_until(master, marker, timeout=5):
    deadline = time.monotonic() + timeout
    output = b""
    while marker not in output and time.monotonic() < deadline:
        if select.select([master], [], [], 0.1)[0]:
            output += os.read(master, 65536)
    assert marker in output, (marker, output[-500:])
    return output
