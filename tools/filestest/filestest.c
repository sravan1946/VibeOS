/* host checks for fsutil.c: run by tools/filestest/run.sh */
#include "fsutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void touch(const char *p, const char *txt, int mode) {
    FILE *f = fopen(p, "w");
    if (txt) fputs(txt, f);
    fclose(f);
    chmod(p, mode);
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static char T[PATH_MAX];
static char *P(const char *rel) { static char b[8][PATH_MAX]; static int i; i = (i + 1) & 7; snprintf(b[i], PATH_MAX, "%s/%s", T, rel); return b[i]; }

int main(void) {
    snprintf(T, sizeof T, "/tmp/filestest.XXXXXX");
    if (!mkdtemp(T)) { perror("mkdtemp"); return 1; }
    mkdir(P("bin"), 0755);
    touch(P("bin/notepad"), "", 0755); touch(P("bin/viewer"), "", 0755); touch(P("bin/play"), "", 0755);
    touch(P("bin/code"), "", 0755); touch(P("bin/midi"), "", 0755); touch(P("bin/browser"), "", 0755);

    char conf[4096], prog[PATH_MAX];
    snprintf(conf, sizeof conf,
        "# comment\n\ntxt,md  %s/bin/notepad\nc,lua\t%s/bin/code\npng,JPG %s/bin/viewer\n"
        "wav,mp3 %s/bin/music|%s/bin/play\nmid %s/bin/midi   \nhtml %s/bin/browser\n* %s/bin/notepad\n",
        T, T, T, T, T, T, T, T);
    CHECK(fs_assoc_text(conf, "/x/a.txt", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/notepad")));
    CHECK(fs_assoc_text(conf, "/x/A.TXT", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/notepad")));
    CHECK(fs_assoc_text(conf, "main.c", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/code")));
    CHECK(fs_assoc_text(conf, "x.lua", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/code")));
    CHECK(fs_assoc_text(conf, "pic.jpg", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/viewer")));
    CHECK(fs_assoc_text(conf, "pic.PNG", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/viewer")));
    CHECK(fs_assoc_text(conf, "s.wav", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/play")));     /* music player absent: falls back to play */
    touch(P("bin/music"), "", 0755);
    CHECK(fs_assoc_text(conf, "s.mp3", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/music")));    /* ...and prefers it once installed */
    CHECK(fs_assoc_text(conf, "t.mid", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/midi")));
    CHECK(fs_assoc_text(conf, "i.html", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/browser")));
    CHECK(fs_assoc_text(conf, "mystery.xyz", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/notepad")));   /* "*" */
    CHECK(fs_assoc_text(conf, "README", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/notepad")));
    CHECK(fs_assoc_text(conf, "/bin/ls", 1, prog, sizeof prog) == 2);                                    /* executable, no extension */
    CHECK(fs_assoc_text(conf, "run.txt", 1, prog, sizeof prog) == 1);                                    /* an extension entry beats the exec bit */
    CHECK(fs_assoc_text(conf, ".hidden", 0, prog, sizeof prog) == 1 && !strcmp(prog, P("bin/notepad")));
    CHECK(fs_assoc_text("txt /nonexistent/prog\n", "a.txt", 0, prog, sizeof prog) == 0);                 /* missing program, no "*" */
    CHECK(fs_assoc_text("", "a.txt", 0, prog, sizeof prog) == 0);
    CHECK(fs_assoc_text("", "a", 1, prog, sizeof prog) == 2);

    CHECK(fs_kind("a.png", 0, 0) == FK_IMAGE && fs_kind("a.WAV", 0, 0) == FK_AUDIO && fs_kind("a.c", 0, 0) == FK_CODE);
    CHECK(fs_kind("a", 1, 0) == FK_DIR && fs_kind("a", 0, 1) == FK_EXE && fs_kind("a.txt", 0, 1) == FK_TEXT && fs_kind("a.zzz", 0, 0) == FK_FILE);

    /* trash */
    fs_trash_dir = strdup(P(".trash"));
    mkdir(P("docs"), 0755);
    touch(P("docs/a.txt"), "hello", 0644);
    mkdir(P("docs/sub"), 0755);
    touch(P("docs/sub/b.txt"), "world", 0644);
    CHECK(fs_trash_count() == 0);
    char name[256], orig[PATH_MAX], dest[PATH_MAX];
    uint32_t when = 0;
    CHECK(fs_trash_put(P("docs/a.txt"), name, sizeof name) == 0 && !strcmp(name, "a.txt"));
    CHECK(!exists(P("docs/a.txt")) && exists(P(".trash/files/a.txt")));
    CHECK(fs_trash_info("a.txt", orig, sizeof orig, &when) == 0 && !strcmp(orig, P("docs/a.txt")) && when > 1000000000u);
    touch(P("docs/a.txt"), "second", 0644);                                      /* same name again: stored under a new name */
    CHECK(fs_trash_put(P("docs/a.txt"), name, sizeof name) == 0 && !strcmp(name, "a (2).txt"));
    CHECK(fs_trash_put(P("docs/sub"), name, sizeof name) == 0 && !strcmp(name, "sub"));
    CHECK(exists(P(".trash/files/sub/b.txt")) && fs_trash_count() == 3);
    CHECK(fs_trash_put(P("docs/nope"), NULL, 0) == -1);
    CHECK(fs_trash_put(P(".trash/files/sub"), NULL, 0) == -1);                   /* already in the trash */
    CHECK(fs_trash_put("/", NULL, 0) == -1);
    CHECK(fs_trash_restore("a.txt", dest, sizeof dest) == 0 && !strcmp(dest, P("docs/a.txt")) && exists(P("docs/a.txt")));
    CHECK(!exists(P(".trash/info/a.txt")) && fs_trash_count() == 2);
    touch(P("docs/a.txt"), "occupied", 0644);                                    /* restore onto an existing name: gets a new one */
    CHECK(fs_trash_restore("a (2).txt", dest, sizeof dest) == 0 && !strcmp(dest, P("docs/a (2).txt")));
    rmdir(P("docs/sub"));
    CHECK(fs_rmrf(P("docs")) == 0 && !exists(P("docs")));                        /* parent gone: restore recreates it */
    CHECK(fs_trash_restore("sub", dest, sizeof dest) == 0 && !strcmp(dest, P("docs/sub")) && exists(P("docs/sub/b.txt")));
    CHECK(fs_trash_restore("sub", dest, sizeof dest) == -1);
    mkdir(P("v"), 0755); touch(P("v/x"), "x", 0644); mkdir(P("v/d"), 0755); touch(P("v/d/y"), "y", 0644);
    CHECK(fs_trash_put(P("v"), NULL, 0) == 0 && fs_trash_put(P("docs"), NULL, 0) == 0 && fs_trash_count() == 2);
    CHECK(fs_trash_empty() == 0 && fs_trash_count() == 0 && !exists(P(".trash/files/v")) && !exists(P(".trash/info/v")));

    /* helpers */
    mkdir(P("src"), 0755); touch(P("src/f1"), "0123456789", 0644); mkdir(P("src/in"), 0755); touch(P("src/in/f2"), "abc", 0644);
    CHECK(fs_copy(P("src"), P("dst")) == 0 && exists(P("dst/in/f2")) && exists(P("dst/f1")));
    CHECK(fs_copy(P("src"), P("src/in/self")) == -1);
    uint32_t nf, nd; uint64_t nb;
    fs_du(P("src"), &nf, &nd, &nb);
    CHECK(nf == 2 && nd == 2 && nb == 13);
    char u[PATH_MAX];
    snprintf(u, sizeof u, "%s", P("src/f1"));
    fs_unique(u, sizeof u);
    CHECK(!strcmp(u, P("src/f1 (2)")));
    touch(P("src/g.tar.gz"), "", 0644);
    snprintf(u, sizeof u, "%s", P("src/g.tar.gz"));
    fs_unique(u, sizeof u);
    CHECK(!strcmp(u, P("src/g.tar (2).gz")));
    snprintf(u, sizeof u, "%s", P("src/new"));
    fs_unique(u, sizeof u);
    CHECK(!strcmp(u, P("src/new")));
    CHECK(fs_mkdirs(P("a/b/c")) == 0 && exists(P("a/b/c")) && fs_mkdirs(P("a/b/c")) == 0);
    CHECK(!strcmp(fs_base("/x/y/z.txt"), "z.txt") && !strcmp(fs_base("z"), "z"));

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", T);
    if (system(cmd)) {}
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
