/* widgets - the toolkit gallery: every ui.h widget in every state, for comparing against docs/DESIGN.md and the
 * mockups. A dev tool, not in the launcher. `widgets menu` opens the dropdown, `widgets dialog` / `widgets danger` a
 * dialog, `widgets toast` keeps a toast up (for screenshots). */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <string.h>
#include <stdio.h>

#define W 1232
#define H 680

static struct ui ui;

static void head(int x, int y, const char *s) { gfx_text(ui.c, T_HEADING, x, y, s, ui_th.text); }
static void cap(int x, int y, const char *s) { gfx_text(ui.c, T_CAPTION, x, y, s, ui_th.text2); }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    struct gui_window *w = gui_window_create("Widgets", W, H, WIN_FLAG_NORESIZE);
    if (!w) { perror("widgets"); return 1; }
    ui_init(&ui, w);
    char t1[64] = "", t2[64] = "notes.txt", t3[64] = "Bad name/", t4[64] = "", q1[64] = "", q2[64] = "wallpaper";
    bool cb[4] = { false, true, true, true }, sw[5] = { false, true, false, true, true };
    int rad = 0, sl[4] = { 50, 30, 70, 40 }, seg = 1, segi = 0, dd[2] = { 0, 2 }, tab = 0, tab2 = 1, side = 0, lsel = 3, lscroll = 2;
    int dlg = !strcmp(mode, "dialog") ? 1 : !strcmp(mode, "danger") ? 2 : 0, opened = 0;
    static const char *const fonts[] = { "Inter 13", "Inter 15", "JetBrains Mono 13", "Noto Sans 13" };
    static const char *const segs[] = { "Day", "Week", "Month" };
    static const int segicons[] = { ICON_GRID, ICON_LIST };
    static const char *const tabs[] = { "General", "Appearance", "Keyboard" };
    static const char *const crumbs[] = { "Home", "Projects", "vibeos" };
    static const char *items[20];
    char ibuf[20][24];
    for (int i = 0; i < 20; i++) { snprintf(ibuf[i], sizeof ibuf[i], "Item %d", i + 1); items[i] = ibuf[i]; }
    if (!strcmp(mode, "toast")) ui_toast(&ui, "Moved \xe2\x80\x9cold-notes.txt\xe2\x80\x9d to Trash", "Undo");

    do {
        struct canvas *c = ui.c;
        ui_clear(&ui);
        /* ---- column 1: buttons, icon buttons, inputs ---- */
        int x = 20, y = 16;
        head(x, y, "Buttons");
        static const char *const kinds[] = { "Primary", "Secondary", "Ghost", "Destructive" };
        static const char *const states[] = { "Default", "Hover", "Pressed", "Focus", "Disabled" };
        static const int force[] = { 0, UI_F_HOVER, UI_F_PRESS, UI_F_FOCUS, 0 };
        y += 28;
        for (int k = 0; k < 4; k++) cap(x + 72 + k * 96, y, kinds[k]);
        y += 20;
        for (int s = 0; s < 5; s++, y += 36) {
            cap(x, y + 7, states[s]);
            for (int k = 0; k < 4; k++) {
                ui.force = force[s];
                static const int st[] = { UI_PRIMARY, UI_SECONDARY, UI_GHOST, UI_DANGER };
                ui_button(&ui, x + 72 + k * 96, y, 88, H_CTL, k == 3 ? "Delete" : "Save", st[k] | (s == 4 ? UI_DISABLED : 0));
                ui.force = 0;
            }
        }
        y += 4;
        cap(x, y + 7, "Icons");
        int bx = x + 72;
        for (int s = 0; s < 6; s++) {
            ui.force = s < 4 ? force[s] : 0;
            ui_icon_button(&ui, bx, y, s == 4 ? ICON_SIDEBAR : ICON_SEARCH, s == 4 ? UI_ON : s == 5 ? UI_DISABLED : 0, "Search · Ctrl+F");
            ui.force = 0;
            bx += 32;
        }
        y += 36;
        cap(x, y + 7, "Icon, label");
        ui_button_ex(&ui, x + 72, y, ui_button_w(ICON_PLUS, "New file"), H_CTL, ICON_PLUS, "New file", UI_SECONDARY);
        ui_button_ex(&ui, x + 80 + ui_button_w(ICON_PLUS, "New file"), y, ui_button_w(ICON_TRASH, "Delete"), H_CTL, ICON_TRASH, "Delete", UI_GHOST);
        ui_button_ex(&ui, x + 72 + 96 * 3, y, 88, H_CTL, UI_NOICON, "Save", UI_PRIMARY | UI_LOADING);
        cap(x + 72 + 96 * 2 + 30, y + 7, "Loading");
        y += 48;
        head(x, y, "Text input");
        y += 28;
        int iw = 200;
        ui_input(&ui, x, y, iw, H_CTL, t1, sizeof t1, "File name", UI_NOICON, 0);
        ui.force = UI_F_HOVER;
        ui_input(&ui, x + iw + 16, y, iw, H_CTL, t4, sizeof t4, "Hover", UI_NOICON, 0);
        ui.force = 0;
        y += 40;
        if (!opened && !ui.focus) ui.focus = ui_id(x, y, iw, H_CTL);       /* show a focused field with its caret */
        ui_input(&ui, x, y, iw, H_CTL, t2, sizeof t2, "File name", ICON_FILE, 0);
        ui_input(&ui, x + iw + 16, y, iw, H_CTL, t3, sizeof t3, "File name", UI_NOICON, UI_ERROR);
        gfx_text(c, T_CAPTION, x + iw + 16, y + H_CTL + 4, "Names can't contain \xe2\x80\x9c/\xe2\x80\x9d", ui_th.danger);
        y += 52;
        ui_input(&ui, x, y, iw, H_CTL, t4, sizeof t4, "Disabled", UI_NOICON, UI_DISABLED);
        y += 40;
        ui_search(&ui, x, y, iw, q1, sizeof q1, "Search settings");
        ui_search(&ui, x + iw + 16, y, iw, q2, sizeof q2, "Search settings");
        y += 48;
        head(x, y, "Tabs");
        y += 28;
        ui_tabbar(&ui, x, y, 416, tabs, 3, &tab, 0);
        y += 40;
        ui_tabbar(&ui, x, y, 416, tabs, 3, &tab2, UI_INACTIVE);
        y += 44;

        /* ---- column 2: toggles, slider, segmented, dropdown ---- */
        x = 476, y = 16;
        head(x, y, "Checkboxes and radios");
        y += 28;
        ui_checkbox(&ui, x, y, "Off", &cb[0]);
        ui_checkbox(&ui, x + 84, y, "On", &cb[1]);
        ui_checkbox_ex(&ui, x + 160, y, "Mixed", &cb[2], UI_MIXED);
        ui_checkbox_ex(&ui, x + 248, y, "Disabled", &cb[3], UI_DISABLED);
        y += 32;
        ui_radio(&ui, x, y, "Light", &rad, 0, 0);
        ui_radio(&ui, x + 84, y, "Dark", &rad, 1, 0);
        ui.force = UI_F_FOCUS;
        ui_radio(&ui, x + 160, y, "Focus", &rad, 2, 0);
        ui.force = 0;
        ui_radio(&ui, x + 248, y, "Disabled", &rad, 3, UI_DISABLED);
        y += 44;
        head(x, y, "Switches");
        y += 30;
        static const char *const swl[] = { "Off", "On", "Hover", "Focus", "Disabled" };
        for (int i = 0; i < 5; i++) {
            ui.force = i == 2 ? UI_F_HOVER : i == 3 ? UI_F_FOCUS : 0;
            ui_switch(&ui, x + i * 72, y, &sw[i], i == 4 ? UI_DISABLED : 0);
            ui.force = 0;
            cap(x + i * 72, y + 26, swl[i]);
        }
        y += 56;
        head(x, y, "Sliders");
        y += 28;
        static const char *const sll[] = { "Default", "Hover", "Focus", "Disabled" };
        for (int i = 0; i < 4; i++, y += 30) {
            cap(x, y + 5, sll[i]);
            ui.force = i == 1 ? UI_F_HOVER : i == 2 ? UI_F_FOCUS : 0;
            ui_slider_ex(&ui, x + 72, y, 200, &sl[i], 0, 100, i == 3 ? UI_DISABLED : 0);
            ui.force = 0;
            char v[16];
            snprintf(v, sizeof v, "%d px", 9 + sl[i] * 8 / 100);
            gfx_text(c, T_BODY, x + 72 + 200 + 16 + 40 - gfx_text_width(T_BODY, v), y + 3, v, i == 3 ? ui_th.disabled : ui_th.text);
        }
        y += 12;
        head(x, y, "Segmented");
        y += 28;
        ui_segmented(&ui, x, y, NULL, segs, 3, &seg);
        ui_segmented(&ui, x + 200, y, segicons, NULL, 2, &segi);
        y += 44;
        head(x, y, "Dropdowns");
        y += 28;
        if (!strcmp(mode, "menu") && !opened) { ui.pop_id = ui_id(x, y, 200, H_CTL); ui.pop_items = fonts; ui.pop_n = 4; ui.pop_sel = dd[0]; ui.pop_hi = 1;
            ui.pop_anchor = (struct rect){ x, y, 200, H_CTL }; ui.focus = ui.pop_id; }
        ui_dropdown(&ui, x, y, 200, fonts, 4, &dd[0], 0);
        ui_dropdown(&ui, x + 212, y, 140, fonts, 4, &dd[1], UI_DISABLED);
        y += 44;
        head(x, y, "Dialogs and toasts");
        y += 28;
        if (ui_button(&ui, x, y, ui_button_w(UI_NOICON, "Rename\xe2\x80\xa6"), H_CTL, "Rename\xe2\x80\xa6", UI_SECONDARY)) dlg = 3;
        if (ui_button(&ui, x + 92, y, ui_button_w(UI_NOICON, "Delete\xe2\x80\xa6"), H_CTL, "Delete\xe2\x80\xa6", UI_SECONDARY)) dlg = 2;
        if (ui_button(&ui, x + 176, y, ui_button_w(UI_NOICON, "Show toast"), H_CTL, "Show toast", UI_SECONDARY))
            ui_toast(&ui, "Moved \xe2\x80\x9cold-notes.txt\xe2\x80\x9d to Trash", "Undo");
        if (ui_toast_action(&ui)) ui_toast(&ui, "Restored \xe2\x80\x9cold-notes.txt\xe2\x80\x9d", NULL);
        y += 44;
        head(x, y, "Progress");
        y += 30;
        ui_progress(&ui, x, y, 160, 60, 100);
        ui_progress(&ui, x + 176, y, 160, -1, 0);
        ui_spinner(&ui, x + 352, y - 6);
        if (!strcmp(mode, "toast") && !ui.toast_until) ui_toast(&ui, "Moved \xe2\x80\x9cold-notes.txt\xe2\x80\x9d to Trash", "Undo");

        /* ---- column 3: sidebar, rows, toolbar, list, empty state ---- */
        x = 860, y = 16;
        head(x, y, "Sidebar and rows");
        y += 28;
        ui_sidebar(&ui, x, y, 200, 236);
        ui_side_label(&ui, x, y + 8, "Places");
        static const int sic[] = { ICON_HOME, ICON_MONITOR, ICON_FILE_TEXT, ICON_DOWNLOAD };
        static const char *const sl2[] = { "Home", "Desktop", "Documents", "Downloads" };
        for (int i = 0; i < 4; i++) {
            ui.force = i == 1 ? UI_F_HOVER : i == 2 ? UI_F_FOCUS : 0;
            if (ui_side_item(&ui, x, y + 26 + i * 28, 200, sic[i], sl2[i], i == 3 ? "2.1 GB" : NULL, side == i)) side = i;
            ui.force = 0;
        }
        ui_side_label(&ui, x, y + 26 + 4 * 28 + 16, "Devices");
        if (ui_side_item(&ui, x, y + 26 + 4 * 28 + 34, 200, ICON_DRIVE, "Disk", "96 MB", side == 4)) side = 4;
        int rx = x + 208, rw = 144;
        ui_row(&ui, rx, y, rw, ICON_FOLDER, NULL, "Normal", NULL, NULL, 0);
        ui.force = UI_F_HOVER;
        ui_row(&ui, rx, y + 28, rw, ICON_FOLDER, NULL, "Hover", NULL, NULL, 0);
        ui.force = 0;
        ui_row(&ui, rx, y + 56, rw, ICON_FOLDER, NULL, "Selected", NULL, "3", UI_SELECTED);
        ui_row(&ui, rx, y + 92, rw, UI_NOICON, appicon_get(APPICON_FILES, 32), "Files", "Browse files", NULL, 0);
        ui_row(&ui, rx, y + 136, rw, UI_NOICON, appicon_get(APPICON_SETTINGS, 32), "Settings", "Change looks", NULL, UI_SELECTED);
        y += 252;
        head(x, y, "Toolbar");
        y += 28;
        ui_toolbar(&ui, x, y, 352);
        int tx = x + 8, ty = y + (H_TOOLBAR - H_CTL) / 2;
        ui_icon_button(&ui, tx, ty, ICON_CHEVRON_LEFT, 0, "Back");
        ui_icon_button(&ui, tx + 32, ty, ICON_CHEVRON_RIGHT, UI_DISABLED, "Forward");
        ui_vsep(&ui, tx + 68, ty + 6, 16);
        ui_crumbs(&ui, tx + 76, ty, 200, crumbs, 3);
        ui_icon_button(&ui, x + 352 - 8 - 28, ty, ICON_MORE, 0, "More");
        y += 56;
        head(x, y, "List with scrollbar");
        y += 28;
        ui_card(&ui, x, y, 160, 148);
        ui.force = UI_F_HOVER;                                       /* keep the overlay bar visible for the gallery */
        ui_list(&ui, x, y + 2, 160, 144, items, 20, &lsel, &lscroll);
        ui.force = 0;
        ui_card(&ui, x + 176, y, 176, 148);
        ui_empty(&ui, x + 176, y, 176, 148, ICON_FOLDER_OPEN, "No files here", NULL, "Open a folder");
        ui_status(&ui, 0, H - H_STATUS, W, "12 items, \xe2\x80\x9cPictures\xe2\x80\x9d selected", "2.1 GB free");

        if (dlg == 1 || dlg == 2) {
            int r = dlg == 1 ? ui_dialog(&ui, "Replace \xe2\x80\x9cnotes.txt\xe2\x80\x9d?", "A file with this name already exists in Documents. Replacing it overwrites its contents.", "Cancel", "Replace", UI_PRIMARY)
                             : ui_dialog(&ui, "Delete 3 files?", "They are removed for good and can't be restored from Trash.", "Cancel", "Delete 3 files", UI_DANGER);
            if (r) dlg = 0;
        } else if (dlg == 3) {
            static char nm[64] = "notes.txt";
            if (ui_prompt(&ui, "Rename file", "Rename", nm, sizeof nm)) dlg = 0;
        }
        opened = 1;
        ui_end(&ui);
    } while (ui_poll(&ui, -1));
    return 0;
}
