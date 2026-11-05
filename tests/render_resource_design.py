#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render proposed terminal layouts from fictional data; not the production UI.

Usage: python3 tests/render_resource_design.py [output-directory]
Requires Pillow, like render_preview.py. Never reads live host/task state.
"""
import html
import json
import math
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
COLORS = {
    'fg': '#cccccc', 'title': '#eeeeee', 'dim': '#606060', 'off': '#888888',
    'cpu': '#556d59', 'mem': '#6c6c4b', 'net': '#5c588d', 'proc': '#805252',
    'green': '#77ca9b', 'yellow': '#cbc06c', 'red': '#dc4c4c', 'cyan': '#74e6fc',
    'selected': '#6a2f2f', 'purple': '#b0a9de',
}
STATE = {'free': ('F', 'green'), 'allocated': ('H', 'red'), 'reserved': ('R', 'yellow'),
         'disabled': ('X', 'off'), 'unknown': ('?', 'off'), 'external': ('E', 'red'), 'error': ('!', 'red')}


class Screen:
    def __init__(self):
        self.width, self.height = 160, 48
        self.cells = [[(' ', COLORS['fg'], '#000000') for _ in range(self.width)] for _ in range(self.height)]

    def text(self, x, y, value, color='fg', bg='#000000'):
        assert 0 <= x and 0 <= y < self.height and x + len(value) <= self.width, (x, y, value)
        for offset, char in enumerate(value):
            self.cells[y][x + offset] = char, COLORS.get(color, color), bg

    def box(self, x, y, width, height, label, color):
        self.text(x, y, '╭' + '─' * (width - 2) + '╮', color)
        for row in range(y + 1, y + height - 1):
            self.text(x, row, '│', color)
            self.text(x + width - 1, row, '│', color)
        self.text(x, y + height - 1, '╰' + '─' * (width - 2) + '╯', color)
        self.text(x + 2, y, ' ' + label + ' ', 'title')

    def meter(self, x, y, width, percent, color):
        filled = int(width * percent / 100)
        self.text(x, y, '■' * filled, color)
        self.text(x + filled, y, '■' * (width - filled), 'dim')

    def graph(self, x, y, width, height, level, color='green', phase=0):
        for column in range(width):
            value = max(0.02, min(1, level + .11 * math.sin(column * .22 + phase) + .07 * math.sin(column * .63)))
            amount = value * height * 4
            for row in range(height):
                units = max(0, min(4, int(amount - (height - row - 1) * 4)))
                if units:
                    self.text(x + column, y + row, (' ', '⣀', '⣤', '⣶', '⣿')[units], color)

    def save(self, directory, name, description):
        directory.mkdir(parents=True, exist_ok=True)
        (directory / (name + '.txt')).write_text('\n'.join(''.join(c[0] for c in row).rstrip() for row in self.cells) + '\n')
        cell_width, cell_height, margin = 10, 20, 12
        width, height = self.width * cell_width + margin * 2, self.height * cell_height + margin * 2
        font = ImageFont.truetype('/usr/share/fonts/dejavu/DejaVuSansMono.ttf', 16)
        braille = ImageFont.truetype('/usr/share/fonts/dejavu/DejaVuSans.ttf', 16)
        picture = Image.new('RGB', (width, height), '#000000')
        draw = ImageDraw.Draw(picture)
        svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img">',
               f'<title>{html.escape(description)}</title>', '<desc>Proposed terminal layout. All values are simulated; this is not a running monitor screenshot.</desc>',
               f'<rect width="{width}" height="{height}" fill="#000000"/>',
               '<g font-family="DejaVu Sans Mono, monospace" font-size="16" xml:space="preserve">']
        for y, row in enumerate(self.cells):
            for x, (char, foreground, background) in enumerate(row):
                left, top = margin + x * cell_width, margin + y * cell_height
                if background != '#000000':
                    draw.rectangle((left, top, left + cell_width - 1, top + cell_height - 1), fill=background)
                    svg.append(f'<rect x="{left}" y="{top}" width="{cell_width}" height="{cell_height}" fill="{background}"/>')
                if char != ' ':
                    draw.text((left, top), char, font=braille if 0x2800 <= ord(char) <= 0x28ff else font, fill=foreground)
                    svg.append(f'<text x="{left}" y="{top + 15}" fill="{foreground}">{html.escape(char)}</text>')
        svg.extend(('</g>', '</svg>'))
        (directory / (name + '.svg')).write_text('\n'.join(svg) + '\n')
        picture.save(directory / (name + '.png'))


def card(screen, x, y, name, cpu, used, available, users, sessions, free, held, reserved, disabled, running, waiting, states, stale=False):
    screen.box(x, y, 77, 21, name + (' | STALE 22s' if stale else ' | ONLINE 1s'), 'off' if stale else 'cpu')
    if stale:
        screen.text(x + 3, y + 3, 'CPU --       MEM --       NPU free --', 'off')
        screen.text(x + 3, y + 6, 'No fresh sample; availability is unknown.', 'off')
        screen.text(x + 3, y + 8, 'USERS --     SESSIONS --    TASKS wait --', 'off')
        screen.text(x + 3, y + 12, 'Last values are not counted as available capacity.', 'dim')
        screen.text(x + 3, y + 18, 'r reconnect', 'off')
        return
    screen.text(x + 2, y + 2, f'CPU {cpu:4.1f}% / 64 cores', 'green' if cpu < 40 else 'yellow')
    screen.graph(x + 2, y + 4, 72, 4, cpu / 100)
    screen.text(x + 2, y + 9, f'MEM {used:3d}/512 GiB                  available {available:3d} GiB')
    screen.meter(x + 2, y + 10, 72, used / 512 * 100, 'red')
    screen.text(x + 2, y + 11, f'LOAD {cpu * .64:5.2f} / {cpu * .58:.2f} / {cpu * .51:.2f}       load/core {cpu / 100:.2f}')
    screen.text(x + 2, y + 13, f'NPU free {free}/8', 'green' if free else 'red')
    screen.text(x + 20, y + 13, f'held {held}    reserved {reserved}    off {disabled}')
    for index, state in enumerate(states):
        symbol, color = STATE[state]
        screen.text(x + 2 + index * 8, y + 15, f'{index}:{symbol}', color)
    screen.text(x + 2, y + 17, f'TASKS run {running} / wait {waiting}', 'yellow' if waiting else 'fg')
    screen.text(x + 37, y + 17, f'USERS {users} / sessions {sessions}')
    screen.text(x + 2, y + 19, 'queue view | hardware health: unknown', 'dim')


def overview(example):
    s = Screen()
    s.text(2, 0, 'pto-btop | RESOURCE LAYOUT PROPOSAL | SIMULATED DATA', 'title')
    s.text(112, 0, '160x48  | stable host order', 'off')
    states = [d['state'] for d in example['devices']]
    card(s, 1, 2, '1 dev-01', 38, 144, 368, 3, 5, 3, 3, 1, 1, 2, 2, states)
    card(s, 81, 2, '2 dev-02', 12, 88, 424, 1, 2, 6, 2, 0, 0, 1, 0, ['free'] * 6 + ['allocated'] * 2)
    card(s, 1, 25, '3 dev-03', 24, 256, 256, 5, 9, 0, 8, 0, 0, 4, 6, ['allocated'] * 8)
    card(s, 81, 25, '4 dev-04', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, [], stale=True)
    s.text(2, 47, 'Tab select  Enter details  u users  |  F free  H held  R reserved  X disabled  ? unknown  |  q quit', 'off')
    return s


def detail(example):
    s = Screen()
    s.text(2, 0, 'pto-btop | dev-01 | DESIGN / SIMULATED DATA', 'title')
    s.text(111, 0, '1-4 host  Esc overview  ? help  q quit', 'off')
    s.box(1, 2, 157, 12, 'cpu', 'cpu')
    s.text(116, 2, ' users 3 / sessions 5 [u] ', 'title')
    s.text(3, 3, 'total 38%    user 27%    system 9%    iowait 2%', 'green')
    s.graph(3, 5, 96, 7, .38)
    s.box(104, 4, 52, 8, '64-core / demo', 'cpu')
    s.text(106, 5, 'CPU  38%                      51 C', 'green')
    s.meter(106, 6, 45, 38, 'green')
    for row in range(3):
        s.text(106, 7 + row, f'C{row:02d}  {31 + row * 7:2d}%   ⣀⣤⣶⣤⣀     C{row + 32:02d}  {22 + row * 3:2d}%', 'green')
    s.text(106, 10, 'Load avg: 24.32 22.04 19.38')
    s.box(1, 16, 34, 17, 'mem', 'mem')
    s.text(3, 18, 'Total:           512 GiB', 'title')
    s.text(3, 20, 'Used:            144 GiB')
    s.meter(3, 21, 28, 28, 'red')
    s.text(3, 23, 'Available:       368 GiB')
    s.graph(3, 24, 28, 3, .72, 'yellow')
    s.text(3, 28, 'Cached:           64 GiB')
    s.graph(3, 29, 28, 2, .5, 'cyan')
    s.box(36, 16, 35, 17, 'disks', 'mem')
    for index, (name, used) in enumerate((('/', 34), ('/data', 56), ('swap', 0))):
        y = 18 + index * 4
        s.text(38, y, f'{name:<13} used {used:2d}%')
        s.meter(38, y + 1, 29, used, 'red')
        s.text(38, y + 2, 'R 2.1 MiB/s W 1.0 MiB/s', 'off')
    s.box(1, 35, 70, 12, 'net | eth0', 'net')
    s.graph(3, 37, 43, 8, .55, 'purple')
    s.text(49, 38, 'download', 'title')
    s.text(49, 40, '6.18 MiB/s', 'purple')
    s.text(49, 42, 'upload', 'title')
    s.text(49, 44, '1.23 MiB/s', 'purple')
    s.box(73, 16, 85, 15, 'cards [focused] | queue view', 'mem')
    s.text(75, 17, 'free 3/8  held 3  reserved 1  off 1  |  run 2 / wait 2', 'green')
    s.text(75, 19, 'ID  GROUP STATE     USER         TASK             AGE', 'title')
    for index, device in enumerate(example['devices']):
        info = device['allocation'] or device['reservation'] or {}
        user = info.get('user') or '-'
        task = info.get('task_id') or '-'
        age = '12m40s' if device['id'] in ('2', '3') else '3m12s' if device['id'] == '5' else '-'
        state = 'HELD' if device['state'] == 'allocated' else device['state'].upper()
        row = f"{device['id']:>2}  {device['group_id']:<5} {state:<9} {user:<12} {task:<16} {age}"
        color = STATE[device['state']][1]
        if device['id'] == '3':
            s.text(74, 20 + index, ' ' * 83, 'title', COLORS['selected'])
            s.text(75, 20 + index, row, 'title', COLORS['selected'])
        else:
            s.text(75, 20 + index, row, color)
    s.text(75, 29, '5 also reserved for carol | health N/A | 1-8/8', 'off')
    s.box(73, 32, 85, 15, 'proc | cpu | filter: all', 'proc')
    s.text(75, 34, '   PID PROGRAM             USER          MEM       CPU%', 'title')
    for index in range(10):
        name = ('python', 'compiler', 'worker')[index % 3]
        user = ('alice', 'bob', 'carol')[index % 3]
        s.text(75, 35 + index, f'{41001 + index:6d} {name:<19} {user:<12} {1400 - index * 95:5d}M    {80 - index * 7:5.1f}', 'green' if index < 5 else 'off')
    s.text(2, 47, 'v cards/proc  Up/Down select  Enter info  f owner proc  u users  o fold cards  [ ] CPU pages  / proc filter', 'off')
    return s


def main():
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'docs/cluster'
    example = json.loads((ROOT / 'docs/cluster/examples/task-submit-monitor.example.json').read_text())
    overview(example).save(output, 'resources-overview', 'Server overview with login counts and queue card availability')
    detail(example).save(output, 'resources-detail', 'btop-style details with cards above processes and login counts in the CPU header')
    print('Rendered two 160x48 design previews; all cell writes are inside the terminal bounds.')


if __name__ == '__main__':
    main()
