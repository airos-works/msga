#!/usr/bin/env python3
"""QMP input/screenshot helper for the msga test VM (see run-vm.sh)."""
import argparse
import os
import json
import pathlib
import socket
import time

ROOT = pathlib.Path(os.environ.get('MSGA_VM_DIR', '/mnt/HaikuWork/tmp/msga-vm'))

class QMP:
    def __init__(self):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.connect(str(ROOT / 'qmp.sock'))
        self.file = self.socket.makefile('rwb', buffering=0)
        self.read()
        self.command('qmp_capabilities')

    def read(self):
        return json.loads(self.file.readline())

    def command(self, name, **args):
        self.file.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
        while True:
            reply = self.read()
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            if 'return' in reply:
                return reply['return']

    def key(self, keys):
        return self.command('human-monitor-command', **{'command-line': 'sendkey ' + keys + ' 30'})

    def type(self, text):
        punctuation = {' ': 'spc', '\n': 'ret', '\t': 'tab', '-': 'minus', '_': 'shift-minus',
            '=': 'equal', '+': 'shift-equal', '/': 'slash', '?': 'shift-slash',
            '.': 'dot', '>': 'shift-dot', ',': 'comma', '<': 'shift-comma',
            ';': 'semicolon', ':': 'shift-semicolon', "'": 'apostrophe',
            '"': 'shift-apostrophe', '[': 'bracket_left', ']': 'bracket_right',
            '{': 'shift-bracket_left', '}': 'shift-bracket_right', '\\': 'backslash',
            '|': 'shift-backslash', '`': 'grave_accent', '~': 'shift-grave_accent',
            '!': 'shift-1', '@': 'shift-2', '#': 'shift-3', '$': 'shift-4',
            '%': 'shift-5', '^': 'shift-6', '&': 'shift-7', '*': 'shift-8',
            '(': 'shift-9', ')': 'shift-0'}
        for char in text:
            key = punctuation.get(char, ('shift-' + char.lower()) if char.isupper() else char)
            self.key(key)
            time.sleep(0.06)

    def click(self, x, y, width, height, button='left'):
        self.command('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / width)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / height)}}])
        # Let the guest consume tablet motion before dispatching the click.
        time.sleep(0.1)
        self.command('input-send-event', events=[{'type': 'btn', 'data': {'button': button, 'down': True}}])
        time.sleep(0.08)
        self.command('input-send-event', events=[{'type': 'btn', 'data': {'button': button, 'down': False}}])

    def move(self, x, y, width, height):
        self.command('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / width)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / height)}}])

    def wheel(self, x, y, width, height, clicks):
        self.move(x, y, width, height)
        time.sleep(0.1)
        button = 'wheel-down' if clicks > 0 else 'wheel-up'
        for _ in range(abs(clicks)):
            self.command('input-send-event', events=[{'type': 'btn', 'data': {'button': button, 'down': True}}])
            self.command('input-send-event', events=[{'type': 'btn', 'data': {'button': button, 'down': False}}])
            time.sleep(0.05)

    def drag(self, x, y, end_x, end_y, width, height):
        self.move(x, y, width, height)
        # Fresh tablet motion also settles the pointer after typing obscures it.
        time.sleep(0.2)
        self.move(x + 1 if x + 1 < width else x - 1, y, width, height)
        time.sleep(0.1)
        self.move(x, y, width, height)
        time.sleep(0.2)
        self.command('input-send-event', events=[{'type': 'btn', 'data': {'button': 'left', 'down': True}}])
        try:
            time.sleep(0.15)
            for step in range(1, 21):
                self.move(x + (end_x - x) * step / 20, y + (end_y - y) * step / 20, width, height)
                time.sleep(0.04)
        finally:
            self.command('input-send-event', events=[{'type': 'btn', 'data': {'button': 'left', 'down': False}}])

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('action', choices=['status', 'screenshot', 'key', 'type', 'click', 'right-click', 'drag', 'wheel', 'move'])
    p.add_argument('args', nargs='*')
    a = p.parse_args()
    q = QMP()
    if a.action == 'status':
        print(q.command('query-status'))
    elif a.action == 'screenshot':
        path = pathlib.Path(a.args[0]) if a.args else ROOT / 'screen.png'
        q.command('screendump', filename=str(path.resolve()), format='png')
        print(path)
    elif a.action == 'key':
        q.key(a.args[0])
    elif a.action == 'type':
        q.type(a.args[0])
    elif a.action in ['click', 'right-click']:
        q.click(*map(int, a.args), button='right' if a.action == 'right-click' else 'left')
    elif a.action == 'wheel':
        q.wheel(*map(int, a.args))
    elif a.action == 'move':
        q.move(*map(int, a.args))
    elif a.action == 'drag':
        q.drag(*map(int, a.args))
