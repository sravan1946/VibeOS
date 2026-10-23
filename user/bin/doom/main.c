/* doom - doomgeneric on VibeOS. Finds the IWAD (Freedoom, see tools/getwad.sh), keeps config and
 * savegames in /home/.doom, then runs the game loop. The platform layer is doomgeneric_vibeos.c. */
#include <gui.h>
#include <design.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "doomgeneric.h"

#define WAD_DIR "/usr/share/doom/"
static const char *const wads[] = { "freedoom1.wad", "freedoom2.wad", "doom1.wad", "doom.wad", "doom2.wad" };

static bool exists(const char *p) { FILE *f = fopen(p, "r"); if (f) fclose(f); return f != NULL; }

/* started from the launcher there is no terminal to read the message on: say it in a window too */
static void complain(const char *msg) {
    fprintf(stderr, "doom: %s\n", msg);
    if (isatty(2)) return;
    struct gui_window *w = gui_window_create("Doom", 520, 116, WIN_FLAG_NORESIZE);
    if (!w) return;
    char line[300];                                  /* plain gfx: the ui toolkit would pull its icon masks into doom */
    snprintf(line, sizeof line, "%s.", msg);
    if (line[0] >= 'a' && line[0] <= 'z') line[0] -= 32;
    struct gui_event ev;
    do {
        struct canvas *c = &w->cv;
        int y = SP_6;
        gfx_fill(c, 0, 0, w->w, w->h, ui_th.window);
        gfx_text(c, T_HEADING, SP_6, y, "No game data", ui_th.text);
        y += T_HEADING->height + SP_3;
        gfx_text(c, T_BODY, SP_6, y, line, ui_th.text2);
        gfx_text(c, T_BODY, SP_6, y + T_BODY->height, "On the build machine, run make doom-wad, then make.", ui_th.text2);
        gui_flip(w);
    } while (gui_next_event(w, &ev, -1) == 1 && ev.type != EV_CLOSE);
}

int main(int argc, char **argv) {
    static char *av[64], path[256], cwd[200];
    int n = 0, have_iwad = 0, have_mb = 0;
    if (argc > 56) argc = 56;
    getcwd(cwd, sizeof cwd);
    for (int i = 0; i < argc; i++) {
        av[n++] = argv[i];
        if (!strcmp(argv[i], "-mb")) have_mb = 1;
        if (!strcmp(argv[i], "-iwad") && i + 1 < argc) {   /* relative paths: we chdir below */
            have_iwad = 1;
            const char *a = argv[++i];
            if (a[0] != '/') { snprintf(path, sizeof path, "%s/%s", cwd, a); a = path; }
            av[n++] = strdup(a);
            if (!exists(a)) { char m[300]; snprintf(m, sizeof m, "%s: cannot open", a); complain(m); return 1; }
        }
    }
    if (!have_iwad) {
        const char *found = NULL;
        for (unsigned i = 0; i < sizeof wads / sizeof *wads && !found; i++) {
            snprintf(path, sizeof path, WAD_DIR "%s", wads[i]);
            if (exists(path)) found = path;
        }
        if (!found) { complain("no WAD in " WAD_DIR " (or pass -iwad FILE)"); return 1; }
        av[n++] = "-iwad"; av[n++] = path;
    }
    if (!have_mb) { av[n++] = "-mb"; av[n++] = "16"; }
    av[n++] = "-nogui";
    av[n] = NULL;
    mkdir("/home/.doom", 0755);
    chdir("/home/.doom");              /* doomgeneric keeps default.cfg and .savegame/ in the cwd */
    doomgeneric_Create(n, av);
    for (;;) doomgeneric_Tick();
}
