# paint.py - a small paint program on the gui module: drag to draw, click a swatch to pick a colour,
# wheel or [ ] change the brush size, C clears, Esc or the close button quits
import gui

W, H, BAR = 640, 440, 40
COLORS = [0x111111, 0xffffff, 0xe5484d, 0xf5a524, 0xf7e05a, 0x46c37b, 0x3aa5ff, 0x7c5cff, 0xff6fb5, 0x8b6f4e]
PAPER, CHROME = 0xfdfcf7, 0x23223a

def swatch(i):
    return 10 + i * 30, 8, 24, 24

def toolbar(w, color, size):
    w.fill(0, 0, W, BAR, CHROME)
    for i, c in enumerate(COLORS):
        x, y, sw, sh = swatch(i)
        w.fill(x, y, sw, sh, c)
        if c == color:
            w.rect(x - 3, y - 3, sw + 6, sh + 6, 0xffffff, fill=False)
    w.text(W - 150, 8, "brush %2d" % size, 0xede9ff, "ui")
    w.circle(W - 28, BAR // 2, max(1, size // 2), color)
    w.line(0, BAR, W, BAR, 0x4a4770)

def main():
    w = gui.Window("Python Paint", W, H, resizable=False)
    w.fill(PAPER)
    color, size, last = COLORS[0], 8, None
    toolbar(w, color, size)
    w.present()
    while True:
        dirty = False
        for ev in w.events(40):
            if ev.type == "close" or (ev.type == "key" and ev.pressed and ev.key == gui.KEY_ESC):
                return
            if ev.type == "mouse_down" and ev.y < BAR:
                for i, c in enumerate(COLORS):
                    x, y, sw, sh = swatch(i)
                    if x <= ev.x < x + sw and y <= ev.y < y + sh:
                        color, dirty = c, True
            elif ev.type in ("mouse_down", "mouse_move") and ev.button & 1 and ev.y >= BAR:
                if last:
                    w.line(last[0], last[1], ev.x, ev.y, color, width=size)
                w.circle(ev.x, ev.y, max(1, size // 2), color)
                last, dirty = (ev.x, ev.y), True
            elif ev.type == "mouse_up" or (ev.type == "mouse_move" and not ev.button & 1):
                last = None
            elif ev.type == "scroll":
                size, dirty = max(1, min(40, size + (1 if ev.dz < 0 else -1))), True
            elif ev.type == "key" and ev.pressed:
                if ev.ch in "[]":
                    size, dirty = max(1, min(40, size + (2 if ev.ch == "]" else -2))), True
                elif ev.ch in "cC":
                    w.fill(0, BAR, W, H - BAR, PAPER)
                    dirty = True
        if dirty:
            toolbar(w, color, size)
            w.present()

main()
