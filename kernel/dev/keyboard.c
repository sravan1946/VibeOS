/* keyboard.c - PS/2 keyboard, scancode set 1 (as translated by the i8042) */
#include "arch.h"
#include "tty.h"

void (*key_sink)(const struct gui_event *ev);
static int mods, caps, e0;

static const char map_lo[128] =
    "\0\x1b" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char map_hi[128] =
    "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static int special(uint8_t sc, bool ext) {
    if (ext) switch (sc) {
        case 0x48: return KEY_UP;    case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;  case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;  case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;  case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INSERT; case 0x53: return KEY_DELETE;
        case 0x1D: return KEY_RCTRL; case 0x38: return KEY_RALT;
        case 0x5B: case 0x5C: return KEY_SUPER;
        case 0x5D: return KEY_MENU;
        case 0x1C: return KEY_ENTER;
        case 0x35: return '/';
        default: return 0;
    }
    switch (sc) {
    case 0x2A: return KEY_LSHIFT; case 0x36: return KEY_RSHIFT;
    case 0x1D: return KEY_LCTRL;  case 0x38: return KEY_LALT;
    case 0x3A: return KEY_CAPSLOCK;
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: return KEY_F1 + (sc - 0x3B);
    case 0x57: return KEY_F11; case 0x58: return KEY_F12;
    case 0x0E: return KEY_BACKSPACE;
    case 0x01: return KEY_ESC;
    case 0x0F: return KEY_TAB;
    case 0x1C: return KEY_ENTER;
    }
    return 0;
}

static void kbd_irq(struct regs *r) {
    (void)r;
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) { e0 = 1; return; }
    bool ext = e0, rel = sc & 0x80;
    e0 = 0;
    sc &= 0x7F;
    struct gui_event ev = { .type = EV_KEY, .pressed = !rel };
    int key = special(sc, ext);
    int bit = 0;
    switch (key) {
    case KEY_LSHIFT: case KEY_RSHIFT: bit = MOD_SHIFT; break;
    case KEY_LCTRL: case KEY_RCTRL: bit = MOD_CTRL; break;
    case KEY_LALT: case KEY_RALT: bit = MOD_ALT; break;
    case KEY_SUPER: bit = MOD_SUPER; break;
    case KEY_CAPSLOCK: if (!rel) caps ^= 1; break;
    }
    if (bit) mods = rel ? (mods & ~bit) : (mods | bit);
    if (!key && !ext && sc < 128) {
        char lo = map_lo[sc];
        if (!lo) return;
        bool shift = mods & MOD_SHIFT;
        if (caps && lo >= 'a' && lo <= 'z') shift = !shift;
        key = lo;
        ev.ch = shift ? map_hi[sc] : lo;
    } else if (key == KEY_ENTER) ev.ch = '\n';
    else if (key == KEY_BACKSPACE) ev.ch = 127;
    else if (key == KEY_TAB) ev.ch = '\t';
    else if (key == KEY_ESC) ev.ch = 27;
    else if (key == '/') ev.ch = '/';
    if (!key) return;
    if ((mods & MOD_CTRL) && ev.ch) {
        char c = ev.ch;
        if (c >= 'a' && c <= 'z') ev.ch = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') ev.ch = c - 'A' + 1;
        else if (c == '[') ev.ch = 27;
        else if (c == '\\') ev.ch = 28;
        else if (c == ']') ev.ch = 29;
        else if (c == ' ') ev.ch = 0;
    }
    ev.key = key;
    ev.mods = mods;
    if (key_sink) key_sink(&ev);
}

int keyboard_mods(void) { return mods; }

void keyboard_init(void) {
    while (inb(0x64) & 1) inb(0x60);   /* flush */
    irq_register(1, kbd_irq);
}
