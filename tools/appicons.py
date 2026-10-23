"""VibeOS app icons: object silhouettes on a 48px grid, shared muted palette.

Each icon is 3-4 flat tones, a 1px outline, a darker 3px "front" edge at the bottom,
light from above. Style differs per direction only in the outline treatment.
Copied from the redesign mockups (vibeos-design/mockups/src); VibeOS uses Style("tone", on_dark=True)
(direction A). tools/mkicons.py turns these into build/gen/{appicons,fileicons}.c.
"""

SLATE, SLATE_D = "#5B6B7A", "#3E4A56"
SAND, SAND_D = "#D9C7A3", "#B39C70"
SAGE, SAGE_D = "#7FA58A", "#557A60"
BRICK, BRICK_D = "#C4674F", "#93473A"
OCHRE, OCHRE_D = "#E0A645", "#B07A20"
BLUE, BLUE_D = "#4A6FB5", "#2F4C86"
PAPER, PAPER_D = "#F4F5F6", "#D3D6DA"
INK, INK_D = "#2B2D31", "#1E2023"
METAL, METAL_D = "#8A8D94", "#6B6E75"


def _hex(c):
    c = c.lstrip("#")
    return [int(c[i:i + 2], 16) for i in (0, 2, 4)]


def mix(a, b, t):
    A, B = _hex(a), _hex(b)
    return "#%02X%02X%02X" % tuple(round(A[i] + (B[i] - A[i]) * t) for i in range(3))


class Style:
    """outline: 'tone' (30% darker, elementary), 'ink' (printed), 'bold' (BeOS sticker)."""

    def __init__(self, outline="tone", on_dark=False):
        self.kind = outline
        self.on_dark = on_dark
        self.sw = {"tone": 1, "ink": 1, "bold": 1.5}[outline]

    def ol(self, fill):
        r, g, b = _hex(fill)
        if self.on_dark and (0.2126 * r + 0.7152 * g + 0.0722 * b) < 90:
            return mix(fill, "#FFFFFF", 0.22)  # dark objects on dark ground get a light rim
        if self.kind == "tone":
            return mix(fill, "#000000", 0.30)
        if self.kind == "ink":
            return mix(fill, "#16171A", 0.62)
        return "#18191B"


def build(st):
    def o(f):
        return f'fill="{f}" stroke="{st.ol(f)}" stroke-width="{st.sw}" stroke-linejoin="round"'

    def slab(x, y, w, h, r, fill, dark, depth=3):
        # darker body with outline, lighter face on top: the bottom "front edge"
        return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" {o(dark)}/>'
                f'<rect x="{x}" y="{y}" width="{w}" height="{h - depth}" rx="{r}" fill="{fill}"/>')

    def disc(cx, cy, r, fill, dark, depth=2.5):
        return (f'<circle cx="{cx}" cy="{cy}" r="{r}" {o(dark)}/>'
                f'<circle cx="{cx}" cy="{cy - depth / 2}" r="{r - depth / 2}" fill="{fill}"/>')

    def line(d, c, w=2, cap="round"):
        return f'<path d="{d}" fill="none" stroke="{c}" stroke-width="{w}" stroke-linecap="{cap}" stroke-linejoin="round"/>'

    def sheet(fill, fold, lines_html="", x=9.5, y=4.5, w=29, h=39):
        f = 9
        body = (f'<path d="M{x + 2} {y}h{w - f - 2}l{f} {f}v{h - f - 2}a2 2 0 0 1-2 2h-{w - 4}a2 2 0 0 1-2-2v-{h - 4}a2 2 0 0 1 2-2z" {o(fill)}/>'
                f'<path d="M{x + w - f} {y}v{f - 2}a2 2 0 0 0 2 2h{f - 2}z" {o(fold)}/>')
        return body + lines_html

    def folder(c, cd, emblem=""):
        return (f'<path d="M5.5 10.5a2 2 0 0 1 2-2h10l4 4h19a2 2 0 0 1 2 2v26a2 2 0 0 1-2 2h-33a2 2 0 0 1-2-2z" {o(cd)}/>'
                f'<rect x="9" y="14.5" width="30" height="10" rx="1" fill="{mix(c, "#FFFFFF", .55)}"/>'
                + slab(5.5, 18.5, 37, 24, 2.5, c, cd, 3) + emblem)

    I = {}
    I["terminal"] = (
        slab(4.5, 7.5, 39, 30, 4, "#3A3D42", INK, 3)
        + '<rect x="8" y="11" width="32" height="21" rx="2" fill="#17181A"/>'
        + '<path d="M10 11h28a2 2 0 0 1 2 2v6H8v-6a2 2 0 0 1 2-2z" fill="#FFFFFF" opacity=".05"/>'
        + line("M13.5 17.5l5 4-5 4", SAGE, 2.5) + line("M21.5 26h7", PAPER, 2.5)
        + f'<path d="M19 37.5h10l2 5H17z" {o(METAL)}/>'
        + f'<rect x="13.5" y="41.5" width="21" height="3.5" rx="1.5" {o(METAL_D)}/>')
    I["browser"] = (
        disc(24, 24, 19.5, BLUE, BLUE_D, 3)
        + f'<path d="M12 15.5c3 1 5 3.5 4 6.5s-4 2.5-5 5.5 1.5 5.5-1 6.5c-3-3-5-7-5-10s1.5-6 7-8.5z" fill="{SAGE}"/>'
        + f'<path d="M27 8.6c2 2 5 2.5 6.5 5s-1 4-3.5 5.5-1.5 4.5 1 6 6 1 8 3c.5-1.2.5-3 .5-4 0-8-5-14-12.5-15.5z" fill="{SAGE}"/>'
        + f'<path d="M24 22.5c2-.5 4 1 3.5 3.5s-2 3 -1.5 6-2 5-4.5 4.5c-1-3 1-5-1-7.5s1.5-6 3.5-6.5z" fill="{SAGE}"/>'
        + f'<ellipse cx="24" cy="22.75" rx="8" ry="18.25" fill="none" stroke="#FFFFFF" stroke-opacity=".28" stroke-width="1.2"/>'
        + f'<path d="M5.5 22.75h37M8 13.5h32M8 32h32" stroke="#FFFFFF" stroke-opacity=".22" stroke-width="1.2" fill="none"/>')
    I["files"] = folder(SAND, SAND_D)
    I["editor"] = (
        sheet(PAPER, PAPER_D,
              line("M15 18h16M15 23.5h16M15 29h10", BLUE, 2))
        + f'<path d="M37.5 24.5l4 4-12 12-5.5 1.5 1.5-5.5z" {o(OCHRE)}/>'
        + f'<path d="M37.5 24.5l4 4-2.5 2.5-4-4z" {o(BRICK)}/>'
        + f'<path d="M25.5 37l3.5 3.5-5 1.5z" fill="{INK}"/>')
    I["paint"] = (
        f'<path d="M24 6.5c10.5 0 19 6.5 19 15.5 0 5-3.5 7.5-7.5 7-3.5-.5-6 1-5.5 4.5.6 4-1.5 7-6 7-10.5 0-19-7.5-19-17S13.5 6.5 24 6.5z" {o(SAND)}/>'
        + f'<path d="M24 9.5c9 0 16 5.5 16 12.5 0 3.5-2.2 5-4.6 4.5-5.5-1-10 1.5-9 7.2.4 2.4-.8 4.3-3.6 4.3-8.6 0-16-6.3-16-14S15 9.5 24 9.5z" fill="{mix(SAND, "#FFFFFF", .35)}"/>'
        + f'<circle cx="14.5" cy="22" r="3" {o(BRICK)}/><circle cx="19" cy="14.5" r="3" {o(BLUE)}/>'
        + f'<circle cx="28" cy="13" r="3" {o(BRICK_D)}/><circle cx="35" cy="18" r="3" {o(BLUE_D)}/>'
        + f'<path d="M30 42.5l9.5-14 3 2-9.5 14z" {o(INK)}/><path d="M39.5 28.5l3-4.5 3 2-3 4.5z" {o(METAL)}/>')
    I["calc"] = (
        slab(9.5, 4.5, 29, 40, 4, SLATE, SLATE_D, 3)
        + '<rect x="13" y="8" width="22" height="8" rx="1.5" fill="#D9E3DC"/>'
        + line("M27 12h5", INK, 2)
        + "".join(
            f'<rect x="{13 + c * 6}" y="{19.5 + r * 5.5}" width="4" height="3.5" rx="1" fill="{OCHRE if (c == 3 and r >= 2) else PAPER}"/>'
            for r in range(4) for c in range(4)))
    I["clock"] = (
        disc(24, 24, 19.5, SLATE, SLATE_D, 3)
        + f'<circle cx="24" cy="22.5" r="15" fill="{PAPER}"/>'
        + "".join(f'<rect x="23.25" y="9" width="1.5" height="{3 if i % 3 == 0 else 2}" fill="{INK}" transform="rotate({i * 30} 24 22.5)"/>' for i in range(12))
        + line("M24 22.5V14", INK, 2.2) + line("M24 22.5l6 3.5", INK, 2.2)
        + line("M24 22.5l-4.5 9", BRICK, 1.2) + f'<circle cx="24" cy="22.5" r="1.8" fill="{BRICK}"/>')
    I["sysmon"] = (
        disc(24, 24, 19.5, "#3A3D42", INK, 3)
        + f'<path d="M11.3 28.5A13 13 0 0 1 32.7 13.5" fill="none" stroke="{SAGE}" stroke-width="4" stroke-linecap="butt"/>'
        + f'<path d="M32.7 13.5A13 13 0 0 1 36.7 28.5" fill="none" stroke="{BRICK}" stroke-width="4" stroke-linecap="butt"/>'
        + line("M24 25L31 15.5", PAPER, 2.5)
        + f'<circle cx="24" cy="25" r="3" {o(METAL)}/>'
        + f'<rect x="17" y="32" width="14" height="4" rx="1" fill="#17181A"/>')
    snake_d = "M11 38c-4 0-5-5-1-6.5s16 1 18-4-12-5-11-11.5S30 9 36 11"
    I["snake"] = (
        line(snake_d, st.ol(SAGE), 9.5) + line(snake_d, SAGE_D, 7.5) + line(snake_d, SAGE, 5)
        + f'<path d="M34 7.5c4-1.5 8.5 1 8.5 4.5s-4 5-8 4c-2.5-.7-3.5-2.5-3.3-4.3.2-1.8 1.3-3.4 2.8-4.2z" {o(SAGE)}/>'
        + f'<circle cx="38.5" cy="10.5" r="1.4" fill="{INK}"/>'
        + line("M42.5 12.5l3 1.5M45.5 14l1-1.5M45.5 14l.5 2", BRICK, 1.2))

    def blk(x, y, c, cd):
        return f'<rect x="{x}" y="{y}" width="11" height="11" rx="1.5" {o(cd)}/><rect x="{x}" y="{y}" width="11" height="8.5" rx="1.5" fill="{c}"/>'
    I["tetris"] = (
        blk(6.5, 33.5, BLUE, BLUE_D) + blk(18.5, 33.5, BLUE, BLUE_D) + blk(30.5, 33.5, OCHRE, OCHRE_D)
        + blk(18.5, 21.5, BLUE, BLUE_D) + blk(30.5, 21.5, OCHRE, OCHRE_D) + blk(30.5, 9.5, OCHRE, OCHRE_D)
        + blk(6.5, 21.5, mix(BLUE, "#FFFFFF", .35), BLUE))
    spikes = "".join(f'<rect x="21.25" y="12" width="3.5" height="30" rx="1.5" fill="#3A3D42" transform="rotate({a} 23 27)"/>' for a in (0, 45, 90, 135))
    I["mines"] = (
        spikes + f'<circle cx="23" cy="27" r="11" {o("#3A3D42")}/>'
        + f'<circle cx="19.5" cy="23.5" r="3" fill="#FFFFFF" opacity=".55"/>'
        + line("M36 5v18", METAL_D, 2)
        + f'<path d="M37 5.5l8 4-8 4z" {o(BRICK)}/>')
    I["doom"] = (
        f'<path d="M8.5 30c0-14 6.5-23.5 15.5-23.5S39.5 16 39.5 30v6.5a6 6 0 0 1-6 6h-19a6 6 0 0 1-6-6z" {o(SAGE_D)}/>'
        + f'<path d="M8.5 30c0-14 6.5-23.5 15.5-23.5S39.5 16 39.5 30v4H8.5z" fill="{SAGE}"/>'
        + f'<path d="M12 22.5c3-1.5 7.5-2 12-2s9 .5 12 2v7.5c-3 1.5-7.5 2-12 2s-9-.5-12-2z" {o(INK)}/>'
        + f'<path d="M14 24c3-1 6.5-1.3 10-1.3" stroke="#FFFFFF" stroke-opacity=".35" stroke-width="1.5" fill="none" stroke-linecap="round"/>'
        + line("M24 7v12", SAGE_D, 1.5) + f'<rect x="15" y="35" width="18" height="3" rx="1.5" fill="{SAGE_D}"/>')
    I["piano"] = (
        slab(3.5, 11.5, 41, 27, 3, "#3A3D42", INK, 3)
        + f'<rect x="6.5" y="17" width="35" height="17" rx="1" fill="{PAPER}"/>'
        + "".join(f'<rect x="{6.5 + i * 5}" y="17" width=".9" height="17" fill="{PAPER_D}"/>' for i in range(1, 7))
        + "".join(f'<rect x="{6.5 + i * 5 - 1.6}" y="17" width="3.2" height="10" rx=".6" fill="{INK}"/>' for i in (1, 2, 4, 5, 6))
        + '<rect x="8" y="13.5" width="10" height="1.5" rx=".75" fill="#FFFFFF" opacity=".25"/>')
    I["code"] = sheet("#3A4652", SLATE_D,
                      line("M15 17h9M18 22h12M18 27h8M15 32h6", SAGE, 2)
                      + line("M27 17h4M28 27h3", OCHRE, 2))
    I["chess"] = (
        f'<rect x="11.5" y="38.5" width="25" height="5" rx="2" {o(METAL)}/>'
        + f'<path d="M15 38.5c0-5 4-7.5 4-11.5-2.5 1-5 1.5-6.5.5-1.5-1.5-.5-4 2-6.5 2.5-2.5 4-6.5 4.5-10.5l2.5 2 1-4c7 1 12 7 12 15.5 0 6.5-2 9.5-2 14.5z" {o(PAPER)}/>'
        + f'<path d="M32.5 38.5c0-5 2-8 2-14.5 0-6.5-3-11.5-8.5-14 4 3 5.5 8 5.5 14 0 6.5-1.5 9.5-1.5 14.5z" fill="{PAPER_D}"/>'
        + f'<circle cx="21.5" cy="16.5" r="1.5" fill="{INK}"/>')

    def tile(x, y, c, cd):
        return f'<rect x="{x}" y="{y}" width="15" height="15" rx="2" fill="{cd}"/><rect x="{x}" y="{y}" width="15" height="13" rx="2" fill="{c}"/>'
    I["2048"] = (
        slab(5.5, 5.5, 37, 37, 4, SAND_D, mix(SAND_D, "#000000", .2), 3)
        + tile(8.5, 8.5, SAND, SAND_D) + tile(24.5, 8.5, OCHRE, OCHRE_D)
        + tile(8.5, 24.5, mix(SAND, "#FFFFFF", .4), SAND) + tile(24.5, 24.5, SAND, SAND_D)
        )
    heart = "M0 2.2C0 .2 2.6-.6 3.5 1.2 4.4-.6 7 .2 7 2.2 7 4.5 3.5 6.8 3.5 6.8S0 4.5 0 2.2z"
    I["solitaire"] = (
        f'<g transform="rotate(-12 18 30)"><rect x="6.5" y="10.5" width="22" height="30" rx="3" {o(PAPER)}/>'
        f'<path d="{heart}" transform="translate(14 22) scale(1.1)" fill="{BRICK}"/></g>'
        + f'<g transform="rotate(10 30 30)"><rect x="19.5" y="9.5" width="22" height="30" rx="3" {o(PAPER)}/>'
        f'<path d="{heart}" transform="translate(26.5 21) scale(1.2) rotate(180 3.5 3.4)" fill="{INK}"/>'
        f'<path d="M30.7 28.5h1.6l.6 3h-2.8z" fill="{INK}"/></g>')
    import math
    teeth = []
    for i in range(16):
        a = math.radians(i * 22.5 - 5.6)
        r = 11.5 if i % 2 == 0 else 8.7
        for da in (0, 11.2):
            teeth.append((32 + r * math.cos(a + math.radians(da)), 15 + r * math.sin(a + math.radians(da))))
    gear = "M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in teeth) + "z"
    I["settings"] = (
        slab(4.5, 16.5, 31, 27, 4, SLATE, SLATE_D, 3)
        + "".join(line(f"M10 {23 + i * 6.5}h19", "#2F3A44", 2) for i in range(3))
        + f'<circle cx="14" cy="23" r="2.6" {o(PAPER)}/><circle cx="23" cy="29.5" r="2.6" {o(OCHRE)}/><circle cx="17" cy="36" r="2.6" {o(PAPER)}/>'
        + f'<path d="{gear}" {o(METAL)}/><circle cx="32" cy="15" r="4" fill="{SLATE_D}"/>'
        )
    pins = "".join(
        f'<rect x="{15.5 + i * 7}" y="5" width="3" height="6" rx="1" fill="{METAL}"/>'
        f'<rect x="{15.5 + i * 7}" y="37" width="3" height="6" rx="1" fill="{METAL}"/>'
        f'<rect x="5" y="{15.5 + i * 7}" width="6" height="3" rx="1" fill="{METAL}"/>'
        f'<rect x="37" y="{15.5 + i * 7}" width="6" height="3" rx="1" fill="{METAL}"/>' for i in range(3))
    I["about"] = (
        pins + slab(9.5, 9.5, 29, 29, 3.5, SLATE, SLATE_D, 3)
        + f'<rect x="16.5" y="16" width="15" height="13" rx="1.5" {o(OCHRE)}/>'
        + line("M20 22.5h8", OCHRE_D, 1.5))
    I["photos"] = (
        slab(4.5, 7.5, 39, 33, 2.5, PAPER, PAPER_D, 3)
        + '<rect x="8.5" y="11.5" width="31" height="22" fill="#E4EAF3"/>'
        + f'<circle cx="32" cy="17" r="3" fill="{OCHRE}"/>'
        + f'<path d="M8.5 33.5v-6l8-8 7 7 4-3.5 12 10.5z" fill="{BLUE}"/>'
        + f'<path d="M8.5 33.5v-3l8-6 9 9z" fill="{BLUE_D}"/>')
    grooves = "".join(f'<circle cx="24" cy="22.75" r="{r}" fill="none" stroke="#45484E" stroke-width=".8"/>' for r in (11, 13.5, 16))
    I["music"] = (
        disc(24, 24, 19.5, "#33363B", INK, 2.5) + grooves
        + f'<circle cx="24" cy="22.75" r="7" {o(BRICK)}/><circle cx="24" cy="22.75" r="1.6" fill="{INK}"/>'
        + f'<path d="M14 12.5a15 15 0 0 1 8-4" stroke="#FFFFFF" stroke-opacity=".18" stroke-width="1.5" fill="none" stroke-linecap="round"/>')
    I["pypaint"] = (
        slab(4.5, 6.5, 32, 34, 2, PAPER, PAPER_D, 3)
        + line("M9.5 16c4-4 8 4 13 0s6-3 9 0", BLUE, 3)
        + line("M9.5 25c4-4 8 4 13 0", OCHRE, 3)
        + f'<path d="M23.5 44l3-6 3 3z" fill="{OCHRE_D}"/>'
        + f'<path d="M26.5 38l3-3 3 3-3 3z" {o(METAL)}/>'
        + f'<path d="M29.5 35l12-12a2.1 2.1 0 0 1 3 3l-12 12z" {o(BLUE)}/>')

    # file-type icons (Files app and desktop)
    I["folder"] = I["files"]
    I["doc"] = sheet(PAPER, PAPER_D, line("M15 18h16M15 23.5h16M15 29h16M15 34.5h9", METAL, 1.8))
    I["script"] = sheet(PAPER, PAPER_D, line("M15 19l4 3.5-4 3.5", SAGE_D, 2) + line("M21.5 27h6", SAGE_D, 2) + line("M15 33.5h16", METAL, 1.8))
    I["image"] = (
        sheet(PAPER, PAPER_D)
        + f'<rect x="14" y="16" width="20" height="21" rx="1" fill="#E9D9BC"/>'
        + f'<path d="M14 30c4-4 8-5 12-3.5S32 26 34 25v12H14z" fill="{SAND_D}"/>'
        + f'<path d="M14 34c5-3 10-3.5 20-1.5v4.5H14z" fill="{mix(SAND_D, "#000000", .2)}"/>'
        + f'<circle cx="29" cy="20.5" r="2" fill="{OCHRE}"/>')
    # VibeOS additions in the same style: the remaining file kinds
    I["generic"] = sheet(PAPER, PAPER_D)
    I["audio"] = sheet(PAPER, PAPER_D, f'<circle cx="20" cy="33" r="3.5" fill="{SLATE_D}"/>'
                       + line("M23 33V18.5c3 0 6.5 1.5 6.5 5", SLATE_D, 2.2))
    I["code"] = sheet(PAPER, PAPER_D, line("M19 21l-4.5 4.5 4.5 4.5M29 21l4.5 4.5-4.5 4.5", BLUE, 2.2)
                      + line("M26 19.5l-4 12", METAL, 1.8))
    I["web"] = sheet(PAPER, PAPER_D, f'<circle cx="24" cy="27" r="8.5" fill="none" stroke="{BLUE}" stroke-width="1.8"/>'
                     f'<ellipse cx="24" cy="27" rx="3.6" ry="8.5" fill="none" stroke="{BLUE}" stroke-width="1.5"/>'
                     + line("M15.5 27h17", BLUE, 1.5))
    I["archive"] = sheet(PAPER, PAPER_D, "".join(
        f'<rect x="{21 + (i % 2) * 3}" y="{6.5 + i * 3.5}" width="3" height="2" rx=".5" fill="{METAL_D}"/>' for i in range(6))
        + f'<rect x="20" y="27.5" width="8" height="10" rx="1.5" {o(OCHRE)}/>'
        + f'<rect x="22.5" y="32" width="3" height="3.5" rx=".8" fill="{OCHRE_D}"/>')
    I["exec"] = sheet("#3A3D42", INK, line("M15 19l4.5 4-4.5 4", SAGE, 2.2) + line("M22 28.5h8", PAPER, 2.2))
    return I


ORDER = [  # launcher order from /etc/apps, Terminal first
    ("terminal", "Terminal", "Run commands in a shell"),
    ("browser", "Browser", "Open pages on the web"),
    ("files", "Files", "Browse and manage your files"),
    ("editor", "Text Editor", "Write and edit plain text"),
    ("paint", "Paint", "Draw and retouch pictures"),
    ("calc", "Calculator", "Work out sums and conversions"),
    ("clock", "Clock", "Time, date and a stopwatch"),
    ("sysmon", "System Monitor", "CPU, memory and processes"),
    ("snake", "Snake", "Eat, grow, don’t bite your tail"),
    ("tetris", "Tetris", "Stack the falling blocks"),
    ("mines", "Minesweeper", "Clear the field, flag the mines"),
    ("doom", "Doom", "The 1993 shooter, natively"),
    ("piano", "Piano", "Play notes from the keyboard"),
    ("code", "Code", "Edit source with highlighting"),
    ("chess", "Chess", "Play against the computer"),
    ("2048", "2048", "Slide and merge numbered tiles"),
    ("solitaire", "Solitaire", "Klondike patience"),
    ("settings", "Settings", "Change how VibeOS looks"),
    ("about", "About VibeOS", "Version and hardware details"),
    ("photos", "Photos", "View the pictures you keep"),
    ("music", "Music", "Play your music library"),
    ("pypaint", "Python Paint", "A paint app written in Python"),
]


def svg(icons, key, size):
    return (f'<svg width="{size}" height="{size}" viewBox="0 0 48 48" style="display:block;flex:none">'
            f'{icons[key]}</svg>')
