#!/usr/bin/env python3
"""Drives a TUI in a pseudo-terminal: answers the terminal queries, sends keys, keeps the screen with pyte.

    term = Term(["./build/rsv"], cols=120, rows=36, env={...})
    term.wait_for("RUNNING")
    term.send("j"); term.sleep(0.5)
    print(term.text())
    term.png("shot.png")
    term.close()

Only the standard library is needed to run a program and read its output; pyte keeps the screen, Pillow draws PNGs.
"""

import fcntl
import os
import pty
import re
import select
import signal
import struct
import termios
import time

import pyte

# Replies of a modern terminal (like WezTerm) to the queries FTXUI sends when it starts
REPLIES = {
    b"\x1b[c": b"\x1b[?62;22;52c",          # DA1: VT220, 256 colors, clipboard
    b"\x1b[>c": b"\x1b[>1;10;0c",           # DA2
    b"\x1b[>q": b"\x1bP>|WezTerm 20240203\x1b\\",  # XTVERSION
    b"\x1bP$q q\x1b\\": b"\x1bP1$r2 q\x1b\\",      # DECRQSS: cursor shape
}


class _Screen(pyte.Screen):
    """pyte ignores faint (SGR 2), which FTXUI's dim uses: kept in the italics flag (RSV doesn't use italics)"""

    def select_graphic_rendition(self, *attrs, **kwargs):
        mapped = []
        i = 0
        while i < len(attrs):
            a = attrs[i]
            if a in (38, 48) and i + 1 < len(attrs):   # colors: 38;5;n and 38;2;r;g;b are copied as they are
                n = 3 if attrs[i + 1] == 5 else 5
                mapped += attrs[i:i + n]
                i += n
                continue
            mapped += [3] if a == 2 else [22, 23] if a == 22 else [a]
            i += 1
        super().select_graphic_rendition(*mapped, **kwargs)


class Term:
    def __init__(self, argv, cols=120, rows=36, env=None, cwd=None):
        self.cols, self.rows = cols, rows
        self.screen = _Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = bytearray()
        self.pending = b""
        self.osc52 = []
        full_env = dict(os.environ)
        full_env.update({"TERM": "xterm-256color", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"})
        full_env.pop("COLORTERM", None)
        full_env.pop("TERM_PROGRAM", None)
        full_env.update(env or {})
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            if cwd:
                os.chdir(cwd)
            os.execvpe(argv[0], argv, full_env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.started = time.time()
        self.exit_status = None

    # -- output

    def _answer(self, data):
        self.pending += data
        for query, reply in REPLIES.items():
            while query in self.pending:
                os.write(self.fd, reply)
                self.pending = self.pending.replace(query, b"", 1)
        while b"\x1b[6n" in self.pending:
            os.write(self.fd, b"\x1b[1;1R")
            self.pending = self.pending.replace(b"\x1b[6n", b"", 1)
        for m in re.finditer(rb"\x1b\]52;c;([A-Za-z0-9+/=]*)(\x07|\x1b\\)", self.pending):
            self.osc52.append(m.group(1))
        self.pending = self.pending[-64:]

    def pump(self, timeout=0.05):
        """Reads what the program wrote during `timeout` seconds. Returns False once it has exited."""
        end = time.time() + timeout
        alive = True
        while True:
            left = end - time.time()
            r, _, _ = select.select([self.fd], [], [], max(0, left))
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    data = b""
                if not data:
                    alive = False
                    break
                self.raw += data
                self._answer(data)
                self.stream.feed(data.replace(b"\x00", b""))
            elif left <= 0:
                break
        if not alive:
            self._reap()
        return alive

    def _reap(self):
        if self.exit_status is None:
            try:
                _, status = os.waitpid(self.pid, 0)
                self.exit_status = status
            except ChildProcessError:
                self.exit_status = -1

    def sleep(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if not self.pump(min(0.05, max(0.0, end - time.time()))):
                return False
        return True

    def wait_for(self, text, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.text():
                return True
            if not self.pump(0.05):
                break
        return text in self.text()

    def wait_exit(self, timeout=10.0):
        """Seconds until the program exits, or None if it is still running after `timeout`."""
        start = time.time()
        while time.time() - start < timeout:
            if not self.pump(0.05):
                return time.time() - start
        return None

    # -- input

    KEYS = {"up": "\x1b[A", "down": "\x1b[B", "right": "\x1b[C", "left": "\x1b[D", "enter": "\r", "esc": "\x1b",
            "tab": "\t", "backspace": "\x7f", "pgup": "\x1b[5~", "pgdn": "\x1b[6~", "home": "\x1b[H", "end": "\x1b[F",
            "ctrl-c": "\x03", "ctrl-d": "\x04", "ctrl-u": "\x15"}

    def send(self, keys, delay=0.0):
        data = self.KEYS.get(keys, keys)
        os.write(self.fd, data.encode())
        if delay:
            self.sleep(delay)

    def wheel(self, x, y, down=True):
        button = 65 if down else 64
        os.write(self.fd, ("\x1b[<%d;%d;%dM" % (button, x + 1, y + 1)).encode())

    def click(self, x, y):
        os.write(self.fd, ("\x1b[<0;%d;%dM\x1b[<0;%d;%dm" % (x + 1, y + 1, x + 1, y + 1)).encode())

    def resize(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.screen.resize(rows, cols)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def hangup(self):
        """Closes the terminal, like a dropped SSH connection."""
        os.close(self.fd)
        self.fd = os.open(os.devnull, os.O_RDONLY)

    def close(self):
        if self.exit_status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self._reap()
        try:
            os.close(self.fd)
        except OSError:
            pass

    # -- screen

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def lines(self):
        return [line.rstrip() for line in self.screen.display]

    def snapshot(self):
        """A copy of the screen cells, to draw later."""
        return [[(c.data, c.fg, c.bg, c.bold, c.reverse, c.italics) for c in (self.screen.buffer[y][x] for x in range(self.cols))]
                for y in range(self.rows)]


# ---------------------------------------------------------------- PNG

PALETTE = {"black": (28, 28, 30), "red": (232, 93, 93), "green": (126, 211, 125), "brown": (229, 192, 123),
           "yellow": (229, 192, 123), "blue": (97, 175, 239), "magenta": (198, 120, 221), "cyan": (86, 182, 194),
           "white": (220, 223, 228), "brightblack": (110, 115, 125), "brightred": (240, 113, 120),
           "brightgreen": (152, 230, 150), "brightyellow": (240, 210, 140), "brightblue": (125, 190, 245),
           "brightmagenta": (215, 140, 235), "brightcyan": (110, 200, 210), "brightwhite": (250, 250, 250)}
BACKGROUND = (24, 25, 28)
FOREGROUND = (215, 218, 224)


def _rgb(color, default):
    if color in (None, "default"):
        return default
    if isinstance(color, str) and re.fullmatch(r"[0-9a-fA-F]{6}", color):
        return tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))
    return PALETTE.get(color, default)


def _fonts(size):
    from PIL import ImageFont
    candidates = [os.path.expanduser("~/Library/Fonts/JetBrainsMono-Regular.ttf"), "/Library/Fonts/JetBrainsMono-Regular.ttf",
                  "/System/Library/Fonts/Menlo.ttc", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"]
    bold = [os.path.expanduser("~/Library/Fonts/JetBrainsMono-Bold.ttf"),
            os.path.expanduser("~/Library/Fonts/JetBrainsMonoNLNerdFont-Bold.ttf"), "/System/Library/Fonts/Menlo.ttc"]
    cjk = ["/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/PingFang.ttc",
           "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"]

    def first(paths):
        for p in paths:
            if os.path.exists(p):
                try:
                    return ImageFont.truetype(p, size)
                except OSError:
                    continue
        return None
    regular = first(candidates) or ImageFont.load_default()
    return regular, first(bold) or regular, first(cjk) or regular


def render_png(cells, path=None, cell_w=9, cell_h=19, font_size=15, scale=1):
    """Draws a snapshot (see Term.snapshot), saved as a PNG if `path` is given.
    Box, block and the few symbols RSV uses are drawn as shapes, so that lines join like in a terminal."""
    from PIL import Image, ImageDraw
    rows, cols = len(cells), len(cells[0])
    img = Image.new("RGB", (cols * cell_w, rows * cell_h), BACKGROUND)
    d = ImageDraw.Draw(img)
    regular, bold, cjk = _fonts(font_size)
    # Which sides of the cell a box character reaches: left, right, up, down (2 = double line)
    boxes = {"─": (1, 1, 0, 0), "━": (1, 1, 0, 0), "│": (0, 0, 1, 1), "┃": (0, 0, 1, 1), "┌": (0, 1, 0, 1),
             "┐": (1, 0, 0, 1), "└": (0, 1, 1, 0), "┘": (1, 0, 1, 0), "├": (0, 1, 1, 1), "┤": (1, 0, 1, 1),
             "┬": (1, 1, 0, 1), "┴": (1, 1, 1, 0), "┼": (1, 1, 1, 1), "╭": (0, 1, 0, 1), "╮": (1, 0, 0, 1),
             "╰": (0, 1, 1, 0), "╯": (1, 0, 1, 0), "╹": (0, 0, 1, 0), "═": (2, 2, 0, 0), "║": (0, 0, 2, 2),
             "╔": (0, 2, 0, 2), "╗": (2, 0, 0, 2), "╚": (0, 2, 2, 0), "╝": (2, 0, 2, 0)}

    def mix(a, b, t):
        return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

    for y, row in enumerate(cells):
        for x, cell in enumerate(row):
            ch, fg, bg, is_bold, rev = cell[:5]
            faint = len(cell) > 5 and cell[5]
            f = _rgb(fg, FOREGROUND)
            b = _rgb(bg, BACKGROUND)
            if rev:
                f, b = b, f
            if faint:
                f = mix(b, f, 0.55)
            X, Y = x * cell_w, y * cell_h
            if b != BACKGROUND:
                d.rectangle([X, Y, X + cell_w - 1, Y + cell_h - 1], fill=b)
            if not ch or ch == " ":
                continue
            cx, cy = X + cell_w // 2, Y + cell_h // 2
            if ch in boxes:
                left, right, up, down = boxes[ch]
                heavy = ch in "┃━"
                for side, kind in (("l", left), ("r", right), ("u", up), ("d", down)):
                    if not kind:
                        continue
                    offsets = (-1, 1) if kind == 2 else (0,)
                    for o in offsets:
                        if side in "lr":
                            x0, x1 = (X, cx + (1 if kind == 2 else 0)) if side == "l" else (cx - (1 if kind == 2 else 0), X + cell_w)
                            d.line([x0, cy + o, x1, cy + o], fill=f, width=2 if heavy else 1)
                        else:
                            y0, y1 = (Y, cy) if side == "u" else (cy, Y + cell_h)
                            d.line([cx + o, y0, cx + o, y1], fill=f, width=2 if heavy else 1)
                continue
            if ch == "█":
                d.rectangle([X, Y, X + cell_w - 1, Y + cell_h - 1], fill=f)
                continue
            if ch in "▏▎▍▌▋▊▉":
                w = (cell_w * ("▏▎▍▌▋▊▉".index(ch) + 1)) // 8
                d.rectangle([X, Y, X + max(1, w) - 1, Y + cell_h - 1], fill=f)
                continue
            if ch in "░▒▓":
                d.rectangle([X, Y, X + cell_w - 1, Y + cell_h - 1], fill=mix(b, f, {"░": 0.22, "▒": 0.45, "▓": 0.7}[ch]))
                continue
            if 0x2800 < ord(ch[0]) <= 0x28FF:   # braille (the live view's curves): 2 x 4 dots, drawn as squares
                bits = ord(ch[0]) - 0x2800
                for bit, (col, dot) in enumerate(((0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2), (0, 3), (1, 3))):
                    if bits & (1 << bit):
                        x0, y0 = X + col * cell_w // 2, Y + dot * cell_h // 4
                        d.rectangle([x0, y0, x0 + cell_w // 2 - 2, y0 + cell_h // 4 - 2], fill=f)
                continue
            if ch == "■":
                d.rectangle([X + 1, cy - 3, X + cell_w - 2, cy + 3], fill=f)
                continue
            if ch in "●○":
                r = cell_w // 2 - 1
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=f if ch == "●" else None, outline=f)
                continue
            font = bold if is_bold else regular
            if ord(ch[0]) >= 0x2E80:
                font = cjk
            d.text((X, Y + 1), ch, font=font, fill=f)
    if scale != 1:
        img = img.resize((img.width * scale, img.height * scale), Image.LANCZOS)
    if path:
        img.save(path)
    return img


def save_gif(images, path, durations):
    images = [im.convert("P", palette=1, colors=255) if im.mode != "P" else im for im in images]
    images[0].save(path, save_all=True, append_images=images[1:], duration=durations, loop=0, optimize=True, disposal=1)
