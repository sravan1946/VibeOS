/* icons.h - build-time rasterised icons (tools/mkicons.py + tools/svgrast.c -> build/gen/{icons,appicons,fileicons}.c).
 * Symbolic: Lucide (third_party/lucide, ISC + Feather MIT) as A8 coverage masks at 16 px (1.5 px stroke) and 20 px
 * (1.75 px), plus 48 px (Lucide's own 2/24 stroke) for ICON48_LIST, the empty-state icons; tint them at draw time
 * (C_TEXT_2 idle, C_TEXT hover, accent when on).
 * Every icon/size is its own symbol, so a binary links only what it uses: icon_get reads icon_use, a table each binary
 * gets from `mkicons.py --use` (the ICON_* names in its sources; libgui.a's default member holds them all), and
 * appicon_get/fileicon_get are inline switches that reference just the ids and sizes a call site asks for.
 * App and file-type icons: tools/appicons.py artwork as premultiplied ARGB (0xAARRGGBB, colour already scaled by
 * alpha: draw with d = s + d * (255 - a) / 255, not blend()) at 24, 32 and 48 px.
 * The lists below are the single source: X(ID, file) for Lucide, X(ID, key, "/etc/apps name") for apps. */
#pragma once
#include <stdint.h>

#define ICON_LIST(X) \
    X(CLOSE, "x") X(MINIMIZE, "minus") X(MAXIMIZE, "square") X(RESTORE, "copy") \
    X(FULLSCREEN, "maximize-2") X(UNFULLSCREEN, "minimize-2") X(SIDEBAR, "panel-left") X(APP_WINDOW, "app-window") \
    X(GRID, "layout-grid") X(LIST, "list") X(MORE, "ellipsis") X(MORE_V, "ellipsis-vertical") X(MENU, "menu") \
    X(SEARCH, "search") X(PLUS, "plus") X(CHECK, "check") X(FILTER, "filter") \
    X(CHEVRON_UP, "chevron-up") X(CHEVRON_DOWN, "chevron-down") X(CHEVRON_LEFT, "chevron-left") X(CHEVRON_RIGHT, "chevron-right") \
    X(ARROW_UP, "arrow-up") X(ARROW_DOWN, "arrow-down") X(ARROW_LEFT, "arrow-left") X(ARROW_RIGHT, "arrow-right") \
    X(SORT, "arrow-up-down") X(SORT_ASC, "arrow-up-narrow-wide") X(SORT_DESC, "arrow-down-narrow-wide") \
    X(HOME, "house") X(FOLDER, "folder") X(FOLDER_OPEN, "folder-open") X(FOLDER_NEW, "folder-plus") X(FOLDER_CODE, "folder-code") \
    X(FILE, "file") X(FILE_NEW, "file-plus") X(FILE_TEXT, "file-text") X(FILE_CODE, "file-code") X(FILE_IMAGE, "file-image") \
    X(FILE_MUSIC, "file-music") X(FILE_ARCHIVE, "file-archive") X(IMAGE, "image") X(MUSIC, "music") X(TERMINAL, "terminal") \
    X(CODE, "code") X(MONITOR, "monitor") X(DRIVE, "hard-drive") X(TRASH, "trash-2") X(ARCHIVE, "archive") \
    X(DOWNLOAD, "download") X(UPLOAD, "upload") X(CUT, "scissors") X(PASTE, "clipboard-paste") X(SAVE, "save") \
    X(PENCIL, "pencil") X(EYE, "eye") X(EYE_OFF, "eye-off") X(LINK, "link") X(EXTERNAL, "external-link") \
    X(STAR, "star") X(BOOKMARK, "bookmark") X(PIN, "pin") X(REFRESH, "rotate-cw") X(HISTORY, "history") \
    X(USER, "user") X(POWER, "power") X(RESTART, "rotate-ccw") X(LOGOUT, "log-out") X(LOCK, "lock") X(UNLOCK, "lock-open") \
    X(SETTINGS, "settings") X(SLIDERS, "sliders-horizontal") X(PALETTE, "palette") X(WALLPAPER, "wallpaper") \
    X(CLOCK, "clock") X(CALENDAR, "calendar") X(TIMER, "timer") X(INFO, "info") X(ALERT, "circle-alert") \
    X(WARNING, "triangle-alert") X(SUCCESS, "circle-check") X(ERROR, "circle-x") X(BELL, "bell") X(BELL_OFF, "bell-off") \
    X(NETWORK, "network") X(WIFI, "wifi") X(WIFI_OFF, "wifi-off") X(VOLUME, "volume") X(VOLUME_LOW, "volume-1") \
    X(VOLUME_HIGH, "volume-2") X(VOLUME_MUTE, "volume-x") X(SUN, "sun") X(MOON, "moon") X(GLOBE, "globe") \
    X(KEYBOARD, "keyboard") X(CPU, "cpu") X(MEMORY, "memory-stick") X(ACTIVITY, "activity") \
    X(PLAY, "play") X(PAUSE, "pause") X(STOP, "square-stop") X(PREV, "skip-back") X(NEXT, "skip-forward") \
    X(SHUFFLE, "shuffle") X(REPEAT, "repeat") X(REPEAT_ONE, "repeat-1") X(PLAYLIST, "list-music") \
    X(BRUSH, "brush") X(ERASER, "eraser") X(FILL, "paint-bucket") X(PICKER, "pipette") X(LINE, "slash") \
    X(ELLIPSE, "circle") X(TEXT, "type") X(SELECT, "square-dashed") X(MOVE, "move") X(SPRAY, "spray-can") \
    X(UNDO, "undo-2") X(REDO, "redo-2") X(ZOOM_IN, "zoom-in") X(ZOOM_OUT, "zoom-out") X(FIT, "scan") X(EXPAND, "expand") \
    X(REPLACE, "replace") X(MATCH_CASE, "case-sensitive") X(WHOLE_WORD, "whole-word") X(REGEX, "regex") \
    X(ENTER, "corner-down-left") X(BACKSPACE, "delete") X(FLAG, "flag") X(BOMB, "bomb") X(MINE, "virus") X(GAMEPAD, "gamepad-2") \
    X(CALCULATOR, "calculator") \
    X(DIVIDE, "divide") X(PLUS_MINUS, "diff")

/* also baked at 48 px (ui_empty, big placeholders); ui_empty with another icon scales the 20 px mask */
#define ICON48_LIST(X) X(CODE) X(SEARCH) X(TRASH) X(FOLDER_OPEN) X(IMAGE) X(MUSIC) X(PLAYLIST)

/* the launcher's 48 px object icons; key = the /etc/apps icon column, name = the app's display name */
#define APPICON_LIST(X) \
    X(TERMINAL, "terminal", "Terminal") X(BROWSER, "browser", "Browser") X(FILES, "files", "Files") \
    X(EDITOR, "editor", "Text Editor") X(PAINT, "paint", "Paint") X(CALC, "calc", "Calculator") \
    X(CLOCK, "clock", "Clock") X(SYSMON, "sysmon", "System Monitor") X(SNAKE, "snake", "Snake") \
    X(TETRIS, "tetris", "Tetris") X(MINES, "mines", "Minesweeper") X(DOOM, "doom", "Doom") \
    X(PIANO, "piano", "Piano") X(CODE, "code", "Code") X(CHESS, "chess", "Chess") X(G2048, "2048", "2048") \
    X(SOLITAIRE, "solitaire", "Solitaire") X(SETTINGS, "settings", "Settings") X(ABOUT, "about", "About VibeOS") \
    X(PHOTOS, "photos", "Photos") X(MUSIC, "music", "Music") X(PYPAINT, "pypaint", "Python Paint")

/* file-type icons for Files and the desktop (folder variants carry a Lucide emblem) */
#define FILEICON_LIST(X) \
    X(FOLDER, "folder") X(FOLDER_HOME, "folder-home") X(FOLDER_DESKTOP, "folder-desktop") \
    X(FOLDER_DOCUMENTS, "folder-documents") X(FOLDER_DOWNLOADS, "folder-downloads") X(FOLDER_MUSIC, "folder-music") \
    X(FOLDER_PICTURES, "folder-pictures") X(FOLDER_PROJECTS, "folder-projects") X(FOLDER_TRASH, "folder-trash") \
    X(TEXT, "text") X(IMAGE, "image") X(AUDIO, "audio") X(CODE, "code") X(SCRIPT, "script") X(WEB, "web") \
    X(ARCHIVE, "archive") X(EXEC, "exec") X(GENERIC, "generic")

#define ICON_X_(id, ...) ICON_##id,
enum { ICON_LIST(ICON_X_) ICON_COUNT };
#undef ICON_X_
#define APPICON_X_(id, ...) APPICON_##id,
enum { APPICON_LIST(APPICON_X_) APPICON_COUNT };
#undef APPICON_X_
#define FILEICON_X_(id, ...) FILEICON_##id,
enum { FILEICON_LIST(FILEICON_X_) FILEICON_COUNT };
#undef FILEICON_X_
/* same picture, other meaning */
#define ICON_MINUS ICON_MINIMIZE
#define ICON_COPY  ICON_RESTORE
#define ICON_RECT  ICON_MAXIMIZE
#define ICON_EDIT  ICON_PENCIL

struct icon  { uint16_t w, h; const uint8_t *a8; };      /* coverage 0..255, row-major, stride = w */
struct aicon { uint16_t w, h; const uint32_t *argb; };   /* premultiplied */

/* nearest baked size at or below `size` (the smallest if none); NULL for an unknown id or one this binary's sources
 * never name (see icon_use) */
const struct icon  *icon_get(int id, int size);          /* 16, 20 (48 for ICON48_LIST) */
extern const struct icon icon_use[ICON_COUNT][3];        /* per binary: [id][16, 20, 48], w = 0 where not linked */
int appicon_find(const char *key_or_name);               /* APPICON_* (case-insensitive key or name) or -1 */

/* 24, 32, 48; same size rule */
#define ICON_AI_(id, ...) extern const struct aicon appicon24_##id, appicon32_##id, appicon48_##id;
APPICON_LIST(ICON_AI_)
#undef ICON_AI_
#define ICON_FI_(id, ...) extern const struct aicon fileicon24_##id, fileicon32_##id, fileicon48_##id;
FILEICON_LIST(ICON_FI_)
#undef ICON_FI_
static inline const struct aicon *appicon_get(int id, int size) {
#define ICON_AC_(id, ...) case APPICON_##id: return size >= 48 ? &appicon48_##id : size >= 32 ? &appicon32_##id : &appicon24_##id;
    switch (id) { APPICON_LIST(ICON_AC_) }
#undef ICON_AC_
    return 0;
}
static inline const struct aicon *fileicon_get(int id, int size) {
#define ICON_FC_(id, ...) case FILEICON_##id: return size >= 48 ? &fileicon48_##id : size >= 32 ? &fileicon32_##id : &fileicon24_##id;
    switch (id) { FILEICON_LIST(ICON_FC_) }
#undef ICON_FC_
    return 0;
}
