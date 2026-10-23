# paint.py - a small paint program on the gui module: drag to draw, click a swatch to pick a colour,
# wheel or [ ] change the brush size, C clears, Esc or the close button quits.
# The toolbar follows the desktop theme (gui.theme(), repainted on the "theme" event); the paper is content.
import gui

W, H, BAR = 640, 440, 40
COLORS = [0x1b1c1f, 0x8a8d94, 0xffffff, 0xc2453e, 0xd9822b, 0xe3c75a, 0x4f9a5e, 0x3e8f8a, 0x4a7fc1, 0x8a63a8, 0xc2668f, 0x8b5e3c]
PAPER = 0xffffff
SW = 20

def swatch(i):
    return 12 + i * (SW + 8), (BAR - SW) // 2, SW, SW

def toolbar(w, t, color, size):
    w.fill(0, 0, W, BAR - 1, t["window"])
    w.line(0, BAR - 1, W, BAR - 1, t["line"])
    for i, c in enumerate(COLORS):
        x, y, sw, sh = swatch(i)
        if c == color:                                   # selected: a 2px accent ring 1px outside
            w.rect(x - 3, y - 3, sw + 6, sh + 6, t["accent_text"], fill=False)
            w.rect(x - 2, y - 2, sw + 4, sh + 4, t["accent_text"], fill=False)
        w.fill(x, y, sw, sh, c)
        w.rect(x, y, sw, sh, t["line_strong"], fill=False)
    label = "Brush %d px" % size
    tw, th = gui.text_size(label, "ui")
    w.text(W - 56 - tw, (BAR - th) // 2, label, t["text2"], "ui")
    w.fill(W - 48, 0, 48, BAR - 1, t["window"])
    r = max(1, min(size // 2, 14))
    w.circle(W - 28, BAR // 2, r + 1, t["control"])          # a 3:1 edge so dark ink reads on a dark bar
    w.circle(W - 28, BAR // 2, r, color)

def main():
    w = gui.Window("Python Paint", W, H, resizable=False)
    t = gui.theme()
    w.fill(PAPER)
    color, size, last = COLORS[0], 8, None
    toolbar(w, t, color, size)
    w.present()
    while True:
        dirty = False
        for ev in w.events(40):
            if ev.type == "close" or (ev.type == "key" and ev.pressed and ev.key == gui.KEY_ESC):
                return
            if ev.type == "theme":
                t, dirty = gui.theme(), True
            elif ev.type == "mouse_down" and ev.y < BAR:
                for i, c in enumerate(COLORS):
                    x, y, sw, sh = swatch(i)
                    if x - 4 <= ev.x < x + sw + 4 and y - 4 <= ev.y < y + sh + 4:
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
            toolbar(w, t, color, size)
            w.present()

main()
