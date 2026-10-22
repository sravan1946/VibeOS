/* fsutil.c - see fsutil.h */
#include "fsutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>

const char *fs_trash_dir = "/home/.trash";
#define MAXN 4096

const char *fs_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static int ext_in(const char *ext, const char *list) {           /* ext (lower case) in "a,b,c" */
    size_t n = strlen(ext);
    for (const char *p = list; *p; ) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncmp(p, ext, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static void get_ext(const char *name, char *ext, int n) {
    const char *b = fs_base(name), *d = strrchr(b, '.');
    int i = 0;
    if (d && d != b) for (d++; *d && i < n - 1; d++) ext[i++] = tolower((unsigned char)*d);
    ext[i] = 0;
}

int fs_kind(const char *name, int isdir, int isexe) {
    char e[16];
    if (isdir) return FK_DIR;
    get_ext(name, e, sizeof e);
    if (ext_in(e, "png,jpg,jpeg,gif,bmp,ppm,pgm")) return FK_IMAGE;
    if (ext_in(e, "wav,mp3,ogg,flac,aac,mid,midi")) return FK_AUDIO;
    if (ext_in(e, "html,htm,xhtml")) return FK_HTML;
    if (ext_in(e, "c,h,cc,cpp,hpp,lua,py,js,sh,mk,asm,s,rs,go,java,css,json,xml,yml,yaml,toml")) return FK_CODE;
    if (ext_in(e, "txt,md,log,conf,cfg,ini,csv,rtf")) return FK_TEXT;
    return isexe ? FK_EXE : FK_FILE;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* first alternative of "a|b|c" that exists */
static int pick(const char *alts, size_t len, char *prog, int n) {
    for (size_t i = 0; i < len; ) {
        size_t j = i;
        while (j < len && alts[j] != '|') j++;
        char t[PATH_MAX];
        size_t l = j - i < sizeof t - 1 ? j - i : sizeof t - 1;
        memcpy(t, alts + i, l); t[l] = 0;
        if (l && exists(t)) { snprintf(prog, n, "%s", t); return 1; }
        i = j + 1;
    }
    return 0;
}

int fs_assoc_text(const char *conf, const char *path, int isexe, char *prog, int n) {
    char ext[16];
    get_ext(path, ext, sizeof ext);
    prog[0] = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && isexe) return 2;                         /* no extension entry: executables run, then "*" */
        for (const char *s = conf; *s; ) {
            const char *e = strchr(s, '\n');
            size_t ll = e ? (size_t)(e - s) : strlen(s);
            const char *nx = e ? e + 1 : s + ll;
            size_t a = 0;
            while (a < ll && (s[a] == ' ' || s[a] == '\t')) a++;
            if (a < ll && s[a] != '#') {
                size_t b = a;
                while (b < ll && s[b] != ' ' && s[b] != '\t') b++;
                size_t c = b;
                while (c < ll && (s[c] == ' ' || s[c] == '\t')) c++;
                size_t d = ll;
                while (d > c && (s[d - 1] == ' ' || s[d - 1] == '\t' || s[d - 1] == '\r')) d--;
                char exts[160];
                size_t el = b - a < sizeof exts - 1 ? b - a : sizeof exts - 1;
                memcpy(exts, s + a, el); exts[el] = 0;
                for (char *q = exts; *q; q++) *q = tolower((unsigned char)*q);
                int wild = !strcmp(exts, "*");
                if (wild == pass && (wild || (ext[0] && ext_in(ext, exts))) && pick(s + c, d - c, prog, n)) return 1;
            }
            s = nx;
        }
    }
    return 0;
}

int fs_assoc(const char *path, char *prog, int n) {
    struct stat st;
    int isexe = stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
    char *conf = NULL;
    int fd = open("/etc/open.conf", O_RDONLY);
    if (fd >= 0) {
        conf = malloc(MAXN);
        int r = conf ? read(fd, conf, MAXN - 1) : -1;
        if (r < 0) r = 0;
        if (conf) conf[r] = 0;
        close(fd);
    }
    int r = fs_assoc_text(conf ? conf : "*  /bin/notepad\n", path, isexe, prog, n);
    free(conf);
    return r;
}

int fs_open(const char *path) {
    char rp[PATH_MAX], prog[PATH_MAX];
    if (!realpath(path, rp)) snprintf(rp, sizeof rp, "%s", path);
    int r = fs_assoc(rp, prog, sizeof prog);
    if (!r) return -1;
#ifndef FS_HOST
    int nul = open("/dev/null", O_RDWR);
    struct spawn_opts o = { { nul, nul, nul }, NULL, 0 };
    char *argv[3] = { (char *)fs_base(r == 2 ? rp : prog), r == 2 ? NULL : rp, NULL };
    int pid = spawnve(r == 2 ? rp : prog, argv, environ, nul >= 0 ? &o : NULL);
    if (nul >= 0) close(nul);
    return pid;
#else
    return r;
#endif
}

/* ---- files ---- */
int fs_mkdirs(const char *path) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; if (mkdir(p, 0755) && !exists(p)) return -1; *s = '/'; }
    return mkdir(p, 0755) && !exists(p) ? -1 : 0;
}

void fs_unique(char *path, int n) {
    if (!exists(path)) return;
    char base[PATH_MAX], ext[64] = "";
    snprintf(base, sizeof base, "%s", path);
    char *sl = strrchr(base, '/'), *dot = strrchr(base, '.');
    if (dot && (!sl || dot > sl + 1)) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
    for (int i = 2; i < 1000; i++) {
        snprintf(path, n, "%s (%d)%s", base, i, ext);
        if (!exists(path)) return;
    }
}

static char **names_of(const char *dir, int *cnt) {            /* names first, so unlinking while walking is safe */
    DIR *d = opendir(dir);
    char **v = NULL;
    int k = 0;
    *cnt = 0;
    if (!d) return NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char **t = realloc(v, (k + 1) * sizeof *v);
        if (!t) break;
        v = t;
        v[k++] = strdup(de->d_name);
    }
    closedir(d);
    *cnt = k;
    return v;
}

static void free_names(char **v, int k) { for (int i = 0; i < k; i++) free(v[i]); free(v); }

static int rmrf(const char *p, int depth) {
    struct stat st;
    if (stat(p, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    if (depth > 16) return -1;
    int k, rc = 0;
    char **v = names_of(p, &k);
    for (int i = 0; i < k; i++) {
        char c[PATH_MAX];
        snprintf(c, sizeof c, "%s/%s", p, v[i]);
        if (rmrf(c, depth + 1)) rc = -1;
    }
    free_names(v, k);
    return rc ? rc : rmdir(p);
}
int fs_rmrf(const char *path) { return rmrf(path, 0); }

static int copy(const char *src, const char *dst, int depth) {
    struct stat st;
    if (stat(src, &st)) return -1;
    if (S_ISDIR(st.st_mode)) {
        size_t sl = strlen(src);
        if (depth > 16 || (!strncmp(dst, src, sl) && (dst[sl] == '/' || !dst[sl]))) return -1;
        if (mkdir(dst, 0755) && !exists(dst)) return -1;
        int k, rc = 0;
        char **v = names_of(src, &k);
        for (int i = 0; i < k; i++) {
            char a[PATH_MAX], b[PATH_MAX];
            snprintf(a, sizeof a, "%s/%s", src, v[i]);
            snprintf(b, sizeof b, "%s/%s", dst, v[i]);
            if (copy(a, b, depth + 1)) rc = -1;
        }
        free_names(v, k);
        return rc;
    }
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777), rc = 0;
    if (out < 0) { close(in); return -1; }
    char buf[4096];
    int r;
    while ((r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, r) != r) { rc = -1; break; }
    if (r < 0) rc = -1;
    close(in); close(out);
    return rc;
}
int fs_copy(const char *src, const char *dst) { return copy(src, dst, 0); }

static void du(const char *p, uint32_t *f, uint32_t *d, uint64_t *b, int depth) {
    struct stat st;
    if (stat(p, &st) || *f + *d > 50000) return;
    if (!S_ISDIR(st.st_mode)) { (*f)++; *b += st.st_size; return; }
    (*d)++;
    if (depth > 12) return;
    int k;
    char **v = names_of(p, &k);
    for (int i = 0; i < k; i++) {
        char c[PATH_MAX];
        snprintf(c, sizeof c, "%s/%s", p, v[i]);
        du(c, f, d, b, depth + 1);
    }
    free_names(v, k);
}
void fs_du(const char *path, uint32_t *files, uint32_t *dirs, uint64_t *bytes) {
    *files = *dirs = 0; *bytes = 0;
    du(path, files, dirs, bytes, 0);
}

/* ---- trash ---- */
static void tpath(char *o, const char *sub, const char *name) { snprintf(o, PATH_MAX, "%s/%s/%s", fs_trash_dir, sub, name); }

int fs_in_trash(const char *path) {
    size_t n = strlen(fs_trash_dir);
    return !strncmp(path, fs_trash_dir, n) && (path[n] == '/' || !path[n]);
}

int fs_trash_put(const char *path, char *name, int n) {
    char rp[PATH_MAX], dir[PATH_MAX], dst[PATH_MAX], inf[PATH_MAX];
    if (!realpath(path, rp) || fs_in_trash(rp) || !strcmp(rp, "/")) return -1;
    snprintf(dir, sizeof dir, "%s/files", fs_trash_dir);
    if (fs_mkdirs(dir)) return -1;
    snprintf(dir, sizeof dir, "%s/info", fs_trash_dir);
    if (fs_mkdirs(dir)) return -1;
    tpath(dst, "files", fs_base(rp));
    fs_unique(dst, sizeof dst);
    if (rename(rp, dst)) return -1;
    tpath(inf, "info", fs_base(dst));
    FILE *f = fopen(inf, "w");
    if (f) { fprintf(f, "%s\n%u\n", rp, (unsigned)time(NULL)); fclose(f); }
    if (name) snprintf(name, n, "%s", fs_base(dst));
    return 0;
}

int fs_trash_info(const char *name, char *orig, int n, uint32_t *when) {
    char inf[PATH_MAX], b[PATH_MAX + 32];
    tpath(inf, "info", name);
    FILE *f = fopen(inf, "r");
    if (!f) return -1;
    int ok = fgets(b, sizeof b, f) != NULL;
    if (ok) {
        b[strcspn(b, "\r\n")] = 0;
        snprintf(orig, n, "%s", b);
        char t[32] = "";
        if (when) *when = fgets(t, sizeof t, f) ? (uint32_t)strtoul(t, NULL, 10) : 0;
    }
    fclose(f);
    return ok ? 0 : -1;
}

int fs_trash_restore(const char *name, char *dest, int n) {
    char src[PATH_MAX], orig[PATH_MAX], inf[PATH_MAX];
    tpath(src, "files", name);
    if (!exists(src) || fs_trash_info(name, orig, sizeof orig, NULL)) return -1;
    char par[PATH_MAX];
    snprintf(par, sizeof par, "%s", orig);
    char *sl = strrchr(par, '/');
    if (sl && sl != par) { *sl = 0; if (fs_mkdirs(par)) return -1; }
    fs_unique(orig, sizeof orig);
    if (rename(src, orig)) return -1;
    tpath(inf, "info", name);
    unlink(inf);
    if (dest) snprintf(dest, n, "%s", orig);
    return 0;
}

int fs_trash_empty(void) {
    char d[PATH_MAX];
    int rc = 0, k;
    for (int i = 0; i < 2; i++) {
        snprintf(d, sizeof d, "%s/%s", fs_trash_dir, i ? "info" : "files");
        char **v = names_of(d, &k);
        for (int j = 0; j < k; j++) {
            char c[PATH_MAX];
            snprintf(c, sizeof c, "%s/%s", d, v[j]);
            if (fs_rmrf(c)) rc = -1;
        }
        free_names(v, k);
    }
    return rc;
}

int fs_trash_count(void) {
    char d[PATH_MAX];
    int k;
    snprintf(d, sizeof d, "%s/files", fs_trash_dir);
    char **v = names_of(d, &k);
    free_names(v, k);
    return k;
}
