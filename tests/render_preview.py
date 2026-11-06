# SPDX-License-Identifier: Apache-2.0
"""Export the actual demo ANSI frame as PNG; optional Pillow, no live host data.

Usage: python tests/render_preview.py build/cluster/btop-cluster /tmp/detail.png [columns rows] [--overview] [--demo-hosts N]
"""
import argparse
import re
import subprocess
import unicodedata
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from terminal_support import screen

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
parser.add_argument("output", type=Path)
parser.add_argument("columns", nargs="?", type=int, default=120)
parser.add_argument("rows", nargs="?", type=int, default=36)
parser.add_argument("--overview", action="store_true")
parser.add_argument("--demo-hosts", type=int, choices=range(1, 5), default=4)
options = parser.parse_args()
width, height = options.columns, options.rows
frame = subprocess.check_output(
    [options.binary, "--demo", "--demo-hosts", str(options.demo_hosts), *([] if options.overview else ["--detail"]),
     "--once", "--width", str(width), "--height", str(height)], text=True
)
screen(frame, width, height)
base = [(0, 0, 0), (205, 49, 49), (13, 188, 121), (229, 229, 16),
        (36, 114, 200), (188, 63, 188), (17, 168, 205), (229, 229, 229),
        (102, 102, 102), (241, 76, 76), (35, 209, 139), (245, 245, 67),
        (59, 142, 234), (214, 112, 214), (41, 184, 219), (255, 255, 255)]


def palette(index):
    if index < 16:
        return base[index]
    if index >= 232:
        return (8 + (index - 232) * 10,) * 3
    index -= 16
    ramp = [0, 95, 135, 175, 215, 255]
    return ramp[index // 36], ramp[index // 6 % 6], ramp[index % 6]


fg, bg = (204, 204, 204), (0, 0, 0)
cells = [[(" ", fg, bg) for _ in range(width)] for _ in range(height)]
y = x = index = 0
while index < len(frame):
    if frame[index] == "\x1b":
        match = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", frame[index:])
        assert match
        raw, code = match.groups()
        args = [int(n) if n else 0 for n in raw.split(";")] if "?" not in raw else []
        if code in ("H", "f"):
            y, x = max(1, args[0]) - 1, max(1, args[1]) - 1
        elif code == "C":
            x += args[0]
        elif code == "D":
            x -= args[0]
        elif code == "A":
            y -= args[0]
        elif code == "B":
            y += args[0]
        elif code == "m":
            i = 0
            while i < len(args):
                attr = args[i]
                color = None
                if attr == 0:
                    fg, bg = (204, 204, 204), (0, 0, 0)
                elif attr in (38, 48):
                    if args[i + 1] == 2:
                        color = tuple(args[i + 2:i + 5]); i += 4
                    elif args[i + 1] == 5:
                        color = palette(args[i + 2]); i += 2
                    if attr == 38:
                        fg = color
                    else:
                        bg = color
                elif 30 <= attr <= 37:
                    fg = palette(attr - 30)
                elif 40 <= attr <= 47:
                    bg = palette(attr - 40)
                elif 90 <= attr <= 97:
                    fg = palette(attr - 90 + 8)
                elif 100 <= attr <= 107:
                    bg = palette(attr - 100 + 8)
                elif attr == 39:
                    fg = (204, 204, 204)
                elif attr == 49:
                    bg = (0, 0, 0)
                i += 1
        index += len(match[0])
        continue
    char = frame[index]; index += 1
    if char == "\n":
        y += 1; continue
    if char == "\r":
        x = 0; continue
    if unicodedata.combining(char):
        continue
    cells[y][x] = char, fg, bg
    x += 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1

font = ImageFont.truetype("/usr/share/fonts/dejavu/DejaVuSansMono.ttf", 16)
braille_font = ImageFont.truetype("/usr/share/fonts/dejavu/DejaVuSans.ttf", 16)
cell_width, cell_height, margin = 10, 21, 12
image = Image.new("RGB", (width * cell_width + margin * 2, height * cell_height + margin * 2), (0, 0, 0))
draw = ImageDraw.Draw(image)
for row, line in enumerate(cells):
    for col, (char, foreground, background) in enumerate(line):
        left, top = margin + col * cell_width, margin + row * cell_height
        draw.rectangle((left, top, left + cell_width - 1, top + cell_height - 1), fill=background)
        draw.text((left, top), char, font=braille_font if 0x2800 <= ord(char) <= 0x28ff else font, fill=foreground)
image.save(options.output)
print(options.output)
