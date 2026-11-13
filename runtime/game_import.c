// Game files from the player's own NSP / XCI dump: runs the bundled hactool on them (game_import.h)
#include "game_import.h"

#include <ctype.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "paths.h"
#include "platform.h"
#include "romfs_build.h"

#ifdef _WIN32
#define SEP "\\"
#define make_dir(p) _mkdir(p)
#else
#define SEP "/"
#define make_dir(p) mkdir(p, 0755)
#endif

// The Legend of Zelda: Breath of the Wild: base game, its update, The Master Trials, The Champions' Ballad
static const uint64_t TITLE[GI_PARTS] = {0x01007EF00011E000ull, 0x01007EF00011E800ull, 0x01007EF00011F001ull,
                                         0x01007EF00011F002ull};
static const char *const WANT_TYPE[GI_PARTS] = {"Program", "Program", "Data", "Data"};

static atomic_bool g_cancel;
static atomic_intptr_t g_running;  // (the hactool running: gi_cancel, and quitting, end it)

static void end_running(void) {
    PlatProc p = (PlatProc)atomic_exchange(&g_running, 0);
    if (p) plat_proc_kill(p);
}

void gi_cancel(void) {
    atomic_store(&g_cancel, true);
    end_running();
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    if (n < k) return false;
    for (size_t i = 0; i < k; i++)
        if (tolower((unsigned char)s[n - k + i]) != suffix[i]) return false;
    return true;
}

bool gi_is_package(const char *path) {
    return ends_with(path, ".nsp") || ends_with(path, ".xci") || ends_with(path, ".nsz") || ends_with(path, ".xcz");
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static uint64_t file_bytes(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (uint64_t)st.st_size : 0;
}

void gi_find_keys(const char *explicit_path, char *out, size_t n) {
    out[0] = 0;
    if (explicit_path && explicit_path[0]) {
        snprintf(out, n, "%s", explicit_path);
        return;
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0]) home = getenv("USERPROFILE");
#endif
    char p[1100];
    if (home && home[0]) {
        snprintf(p, sizeof p, "%s" SEP ".switch" SEP "prod.keys", home);
        if (exists(p)) snprintf(out, n, "%s", p);
    }
}

static bool tool_path(char *out, size_t n) {
    return rt_resource("hactool" SEP "hactool" PLAT_EXE_SUFFIX, "build" SEP "hactool" SEP "hactool" PLAT_EXE_SUFFIX,
                       out, n);
}

bool gi_tool_present(void) {
    char p[1100];
    return tool_path(p, sizeof p);
}

static void set_why(char *why, size_t wn, const char *fmt, const char *arg) {
    if (why && wn) snprintf(why, wn, fmt, arg ? arg : "");
}

// The last lines of a log, for a failure's details
static void log_tail(const char *log, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(log, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), take = size < (long)n - 1 ? size : (long)n - 1;
    fseek(f, size - take, SEEK_SET);
    size_t got = fread(out, 1, (size_t)take, f);
    out[got] = 0;
    fclose(f);
    for (char *c = out; *c; c++)
        if ((unsigned char)*c < 0x20 && *c != '\n') *c = ' ';
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)size + 1);
    if (b) {
        size_t got = fread(b, 1, (size_t)size, f);
        for (size_t i = 0; i < got; i++)
            if (!b[i]) b[i] = ' ';  // (a NUL some key dumpers leave in a line: as a space, not the end of the text)
        b[got] = 0;
    }
    fclose(f);
    return b;
}

// Runs hactool with the cleaned keys; while it runs, progress gets the bytes under watch (expected: the total).
// The exit code (hactool's isn't reliable: callers check what it made), -1 if it didn't start, -2 cancelled.
static int hactool(GiSet *s, const char *const *args, const char *log, const char *watch, uint64_t base,
                   uint64_t expected, void (*progress)(uint64_t, uint64_t)) {
    char exe[1100], keys[1200], titlekeys[1200];
    if (!tool_path(exe, sizeof exe)) return -1;
    snprintf(keys, sizeof keys, "%s" SEP "prod.keys", s->work);
    snprintf(titlekeys, sizeof titlekeys, "%s" SEP "title.keys", s->work);
    setenv("MNG_TITLE_KEYS", titlekeys, 1);  // (the bundled build reads title keys from there: tools/hactool/portable.patch)
    const char *argv[16] = {exe, "-k", keys};
    int n = 3;
    for (int i = 0; args[i] && n < 15; i++) argv[n++] = args[i];
    argv[n] = NULL;
    if (atomic_load(&g_cancel)) return -2;
    PlatProc p = plat_spawn(argv, log, false);
    if (!p) return -1;
    atomic_store(&g_running, p);
    int rc;
    uint64_t next = 0;
    while ((rc = plat_proc_wait(p, false)) < 0) {
        if (atomic_load(&g_cancel)) {
            end_running();
            return -2;
        }
        uint64_t now = plat_now_ns();
        if (progress && watch && now >= next) {
            uint64_t d = romfs_dir_bytes(watch);
            progress(base + (d < expected ? d : expected), 0);
            next = now + 1000000000ull;
        }
        plat_wait_until_ns(now + 100000000ull);
    }
    atomic_store(&g_running, 0);
    return atomic_load(&g_cancel) ? -2 : rc;
}

// prod.keys as hactool reads it: only `name = hex` lines with whole 16-byte values (it stops at comments, a trailing
// NUL some dumpers write, or a value of another length)
static GiResult clean_keys(const char *src, const char *dst, char *why, size_t wn) {
    char *text = read_all(src);
    if (!text) {
        set_why(why, wn, "%s", src);
        return GI_E_NO_KEYS;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        free(text);
        set_why(why, wn, "can't write %s", dst);
        return GI_E_TOOL;
    }
    bool header = false;
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char name[96], hex[1100];
        size_t k = 0, h = 0;
        char *c = line;
        while (*c == ' ' || *c == '\t') c++;
        while ((isalnum((unsigned char)*c) || *c == '_') && k < sizeof name - 1) name[k++] = (char)tolower((unsigned char)*c++);
        name[k] = 0;
        while (*c == ' ' || *c == '\t') c++;
        if (!k || *c++ != '=') continue;
        while (*c == ' ' || *c == '\t') c++;
        while (isxdigit((unsigned char)*c) && h < sizeof hex - 1) hex[h++] = (char)tolower((unsigned char)*c++);
        hex[h] = 0;
        while (*c == ' ' || *c == '\t' || *c == '\r') c++;
        if (*c || !h || h % 32) continue;  // (a note after the value, or a value of another length)
        fprintf(out, "%s = %s\n", name, hex);
        header |= !strcmp(name, "header_key");
    }
    fclose(out);
    free(text);
    if (!header) {
        set_why(why, wn, "%s", "header_key");
        return GI_E_KEYS_INCOMPLETE;
    }
    return GI_OK;
}

// A common ticket's rights id and (encrypted) title key, as a title.keys line
static void add_ticket(const char *tik, FILE *titlekeys) {
    FILE *f = fopen(tik, "rb");
    if (!f) return;
    uint8_t t[0x400];
    size_t n = fread(t, 1, sizeof t, f);
    fclose(f);
    if (n < 4) return;
    uint32_t type = t[0] << 24 | t[1] << 16 | t[2] << 8 | t[3];  // (big-endian signature type)
    size_t sig = type == 0x10000 || type == 0x10003 ? 0x200 : type == 0x10001 || type == 0x10004 ? 0x100
                 : type == 0x10002 || type == 0x10005 ? 0x3C : 0;
    if (!sig) return;
    size_t body = (4 + sig + 0x3F) & ~(size_t)0x3F;
    if (n < body + 0x170) return;
    for (int i = 0; i < 16; i++) fprintf(titlekeys, "%02x", t[body + 0x160 + i]);
    fprintf(titlekeys, " = ");
    for (int i = 0; i < 16; i++) fprintf(titlekeys, "%02x", t[body + 0x40 + i]);
    fprintf(titlekeys, "\n");
}

static const char *field(const char *info, const char *name, char *out, size_t n) {
    const char *p = strstr(info, name);
    out[0] = 0;
    if (!p) return NULL;
    p += strlen(name);
    while (*p == ' ' || *p == '\t') p++;
    size_t k = 0;
    while (*p && !isspace((unsigned char)*p) && k < n - 1) out[k++] = *p++;
    out[k] = 0;
    return out;
}

GiResult gi_open(GiSet *s, const char *const *packages, int n, const char *keys, const char *work,
                 void (*progress)(uint64_t, uint64_t), char *why, size_t wn) {
    memset(s, 0, sizeof *s);
    atomic_store(&g_cancel, false);
    static bool hooked;
    if (!hooked) hooked = true, atexit(end_running);
    snprintf(s->work, sizeof s->work, "%s", work);
    if (!gi_tool_present()) return GI_E_NO_TOOL;
    for (int i = 0; i < n; i++)
        if (ends_with(packages[i], ".nsz") || ends_with(packages[i], ".xcz")) {
            set_why(why, wn, "%s", packages[i]);
            return GI_E_COMPRESSED;
        }
    plat_remove_tree(s->work);
    if (make_dir(s->work) != 0 && !exists(s->work)) {
        set_why(why, wn, "can't make %s", s->work);
        return GI_E_TOOL;
    }
    char path[1300], log[1300];
    snprintf(path, sizeof path, "%s" SEP "prod.keys", s->work);
    GiResult r = clean_keys(keys, path, why, wn);
    if (r) return r;
    snprintf(path, sizeof path, "%s" SEP "title.keys", s->work);
    FILE *titlekeys = fopen(path, "wb");
    if (!titlekeys) {
        set_why(why, wn, "can't write %s", path);
        return GI_E_TOOL;
    }
    // (title.keys next to prod.keys: for packages without their tickets)
    char tk[1100];
    snprintf(tk, sizeof tk, "%s", keys);
    char *sl = strrchr(tk, '/');
#ifdef _WIN32
    if (strrchr(tk, '\\') > sl) sl = strrchr(tk, '\\');
#endif
    snprintf(sl ? sl + 1 : tk, sizeof tk - (size_t)(sl ? sl + 1 - tk : 0), "title.keys");
    if (exists(tk)) {
        char cleaned[1300];
        snprintf(cleaned, sizeof cleaned, "%s" SEP "title.keys.user", s->work);
        if (clean_keys(tk, cleaned, NULL, 0) != GI_E_NO_KEYS) {
            char *t = read_all(cleaned);
            if (t) fputs(t, titlekeys), free(t);
        }
    }
    snprintf(log, sizeof log, "%s" SEP "hactool.log", s->work);
    uint64_t done = 0;
    for (int i = 0; i < n; i++) {
        char dir[1300], opt[1400];
        snprintf(dir, sizeof dir, "%s" SEP "p%d", s->work, i);
        bool xci = ends_with(packages[i], ".xci");
        snprintf(opt, sizeof opt, "%s%s", xci ? "--securedir=" : "--outdir=", dir);
        const char *args[] = {"-t", xci ? "xci" : "pfs0", opt, packages[i], NULL};
        fprintf(stderr, "[import] unpacking %s\n", packages[i]);
        int rc = hactool(s, args, log, dir, done, file_bytes(packages[i]), progress);
        if (rc == -2) { fclose(titlekeys); return GI_E_CANCELLED; }
        done += file_bytes(packages[i]);
        DIR *d = opendir(dir);
        if (rc < 0 || !d) {
            fclose(titlekeys);
            if (d) closedir(d);
            log_tail(log, why, wn);
            fprintf(stderr, "[import] unpacking %s failed: %s\n", packages[i], why);
            return GI_E_TOOL;
        }
        // which NCA is which (hactool reads the header with the keys)
        struct dirent *e;
        while ((e = readdir(d))) {
            char f[1600];
            snprintf(f, sizeof f, "%s" SEP "%s", dir, e->d_name);
            if (ends_with(e->d_name, ".tik")) { add_ticket(f, titlekeys); continue; }
            if (!ends_with(e->d_name, ".nca")) continue;
            if (ends_with(e->d_name, ".cnmt.nca")) { remove(f); continue; }
            char info_log[1300];
            snprintf(info_log, sizeof info_log, "%s" SEP "info.log", s->work);
            fflush(titlekeys);
            const char *a[] = {"-t", "nca", f, NULL};
            if (hactool(s, a, info_log, NULL, 0, 0, NULL) == -2) { closedir(d); fclose(titlekeys); return GI_E_CANCELLED; }
            char *info = read_all(info_log);
            char tid[40], type[40];
            if (!info || !field(info, "Title ID:", tid, sizeof tid) || !field(info, "Content Type:", type, sizeof type)) {
                log_tail(info_log, why, wn);
                free(info);
                closedir(d);
                fclose(titlekeys);
                fprintf(stderr, "[import] %s: the header doesn't decrypt: %s\n", e->d_name, why);
                return GI_E_KEYS_WRONG;
            }
            // (an update's program NCA carries the base game's title id: its BKTR RomFS tells it apart)
            bool patch = strstr(info, "Patch RomFS") != NULL;
            free(info);
            uint64_t id = strtoull(tid, NULL, 16);
            if (id == TITLE[GI_BASE] && patch) id = TITLE[GI_UPDATE];
            int part = -1;
            for (int k = 0; k < GI_PARTS; k++)
                if (id == TITLE[k] && (!strcmp(type, WANT_TYPE[k]) || (k >= GI_DLC1 && !strcmp(type, "PublicData"))))
                    part = k;
            fprintf(stderr, "[import] %s: %s %s%s\n", e->d_name, tid, type, part < 0 ? " (not needed)" : "");
            if (part < 0 || s->nca[part][0]) {
                if (part < 0 && id != TITLE[GI_BASE] && id != TITLE[GI_UPDATE] && strlen(s->other) + 20 < sizeof s->other &&
                    !strstr(s->other, tid))
                    snprintf(s->other + strlen(s->other), sizeof s->other - strlen(s->other), "%s%s", s->other[0] ? ", " : "", tid);
                remove(f);  // (control, legal, manual NCAs, other titles: not needed, room for what follows)
                continue;
            }
            snprintf(s->nca[part], sizeof s->nca[part], "%s", f);
            s->nca_bytes[part] = file_bytes(f);
        }
        closedir(d);
    }
    fclose(titlekeys);
    return GI_OK;
}

static GiResult failed(GiSet *s, int rc, const char *log, char *why, size_t wn) {
    if (rc == -2) return GI_E_CANCELLED;
    log_tail(log, why, wn);
    fprintf(stderr, "[import] hactool failed (%d): %s\n", rc, why);
    // (no key for a rights-managed NCA: hactool says so about the title key)
    char low[2048];
    snprintf(low, sizeof low, "%s", why);
    for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (strstr(low, "titlekey") || strstr(low, "title key")) return GI_E_TITLEKEY;
    (void)s;
    return GI_E_TOOL;
}

GiResult gi_exefs(GiSet *s, const char *out_dir, char *why, size_t wn) {
    char opt[1200], log[1200], main_nso[1200];
    snprintf(opt, sizeof opt, "--exefsdir=%s", out_dir);
    snprintf(log, sizeof log, "%s" SEP "exefs.log", s->work);
    const char *args[] = {opt, s->nca[GI_UPDATE], NULL};
    int rc = hactool(s, args, log, NULL, 0, 0, NULL);
    snprintf(main_nso, sizeof main_nso, "%s" SEP "main", out_dir);
    return rc >= 0 && exists(main_nso) ? GI_OK : failed(s, rc, log, why, wn);
}

GiResult gi_romfs(GiSet *s, const char *out_dir, void (*progress)(uint64_t, uint64_t), char *why, size_t wn) {
    char base[1200], opt[1200], log[1200], ver[1200];
    snprintf(base, sizeof base, "--basenca=%s", s->nca[GI_BASE]);
    snprintf(opt, sizeof opt, "--romfsdir=%s", out_dir);
    snprintf(log, sizeof log, "%s" SEP "romfs.log", s->work);
    const char *args[] = {base, opt, s->nca[GI_UPDATE], NULL};
    int rc = hactool(s, args, log, out_dir, 0, s->nca_bytes[GI_BASE], progress);
    snprintf(ver, sizeof ver, "%s" SEP "System" SEP "Version.txt", out_dir);
    return rc >= 0 && exists(ver) ? GI_OK : failed(s, rc, log, why, wn);
}

GiResult gi_dlc(GiSet *s, int part, const char *out_dir, void (*progress)(uint64_t, uint64_t), char *why, size_t wn) {
    char opt[1200], log[1200], ver[1200];
    snprintf(opt, sizeof opt, "--romfsdir=%s", out_dir);
    snprintf(log, sizeof log, "%s" SEP "dlc%d.log", s->work, part - GI_DLC1 + 1);
    const char *args[] = {opt, s->nca[part], NULL};
    int rc = hactool(s, args, log, out_dir, 0, s->nca_bytes[part], progress);
    snprintf(ver, sizeof ver, "%s" SEP "System" SEP "AocVersion.txt", out_dir);
    return rc >= 0 && exists(ver) ? GI_OK : failed(s, rc, log, why, wn);
}

void gi_drop(GiSet *s, int part) {
    if (s->nca[part][0]) remove(s->nca[part]);
}

void gi_close(GiSet *s) {
    if (s->work[0]) plat_remove_tree(s->work);  // (the cleaned keys go with it)
}
