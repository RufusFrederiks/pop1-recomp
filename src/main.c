/* Prince of Persia (Mac, 1992) static recomp -- loader and entry.
 *
 * Lays out a flat 68k address space, loads the four CODE segments and the
 * CODE 0 jump table into an A5 world, registers the lifted functions, hands
 * the game its resources and the files on its three disks, then runs the
 * THINK C startup and main exactly as the Segment Loader would have.
 *
 * Reads work/files/ (made by tools/prepare.py from your own disk images):
 * the application's own resource fork for code and resources, and the
 * graphics files on all three disks for the File Manager. Nothing game-derived is compiled in except the lifted code in
 * src/gen/, which is generated locally and never committed.
 *
 *   pop1 [--scale N] [workdir]        (workdir defaults to "work") */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void register_seg_1(uint32_t base);
void register_seg_2(uint32_t base);
void register_seg_3(uint32_t base);
void register_seg_4(uint32_t base);

/* Flat address space. Low memory (system globals) sits at the bottom; the
 * guest stack grows down from STACK_TOP; the application globals sit below
 * A5 (27,112 bytes for this title) with the jump table above it; each CODE
 * segment gets 64 KB (the largest is 24 KB). macrecomp's heap starts at 8 MB. */
enum {
    STACK_TOP = 0x00180000,
    A5_BASE   = 0x00200000,
    SEG_BASE  = 0x00300000,
    SEG_SPAN  = 0x00010000,
    MEM_SIZE  = 0x02000000,             /* 32 MB */
    LM_CURSTACKBASE = 0x0908,
    LM_CURRENTA5    = 0x0904,
};

uint32_t pop_seg_base(int s){ return (s >= 1 && s <= 4) ? SEG_BASE + (uint32_t)(s-1)*SEG_SPAN : 0; }

static const char *g_work = "work";
const char *pop_work_dir(void){ return g_work; }

static uint8_t *slurp(const char *path, long *len){
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);
    if(b && fread(b, 1, (size_t)n, f) != (size_t)n){ free(b); b = NULL; }
    fclose(f);
    if(len) *len = n;
    return b;
}

static uint8_t *must(const char *rel, long *len){
    char p[1024]; snprintf(p, sizeof p, "%s/%s", g_work, rel);
    uint8_t *b = slurp(p, len);
    if(!b){ fprintf(stderr, "missing %s -- run tools/prepare.py first\n", p); exit(2); }
    return b;
}

static uint32_t be32(const uint8_t *p){ return (uint32_t)p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3]; }
static uint16_t be16(const uint8_t *p){ return (uint16_t)(p[0]<<8 | p[1]); }

/* ---- the application's resource fork ----
 *
 * Read straight from the fork file the manifest names (the APPL on disk 1),
 * so the runtime needs nothing but work/files/. Format (Inside Macintosh
 * I-128): header dataOffset(4) mapOffset(4) dataLen(4) mapLen(4); in the map,
 * typeListOffset(2) at +24; the type list is count-1(2), then per type
 * type(4) count-1(2) refListOffset(2); each reference is id(2) nameOffset(2)
 * attrs(1) dataOffset(3) handle(4); each datum is length(4) + bytes. */
typedef struct { char type[5]; int id; const uint8_t *data; int len; } ForkRes;
static ForkRes *g_fork; static int g_nfork;

static const char *app_fork_path(void){
    long n; uint8_t *t = must("files/files.txt", &n);
    char *s = (char*)realloc(t, (size_t)n + 1); s[n] = 0;
    static char path[1100];
    for(char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")){
        char *f[7]; int k = 0;
        for(char *p = line; k < 7; k++){ f[k] = p; p = strchr(p, '\t'); if(!p){ k++; break; } *p++ = 0; }
        if(k == 7 && !strcmp(f[1], "APPL") && strcmp(f[5], "-")){
            snprintf(path, sizeof path, "%s/%s", g_work, f[5]); free(s); return path; }
    }
    fprintf(stderr, "no application in %s/files/files.txt -- run tools/prepare.py\n", g_work);
    exit(2);
}

static void load_fork(void){
    const char *path = app_fork_path();
    long n; uint8_t *b = slurp(path, &n);   /* kept for the whole run */
    if(!b || n < 16){ fprintf(stderr, "cannot read %s\n", path); exit(2); }
    uint32_t doff = be32(b), moff = be32(b + 4);
    const uint8_t *map = b + moff, *tl = map + be16(map + 24);
    int ntypes = be16(tl) + 1;
    for(int t = 0; t < ntypes; t++){
        const uint8_t *te = tl + 2 + t*8;
        int cnt = be16(te + 4) + 1;
        const uint8_t *rl = tl + be16(te + 6);
        for(int r = 0; r < cnt; r++){
            const uint8_t *re = rl + r*12;
            uint32_t at = doff + ((uint32_t)re[5] << 16 | (uint32_t)re[6] << 8 | re[7]);
            if(at + 4 > (uint32_t)n) continue;
            g_fork = (ForkRes*)realloc(g_fork, (size_t)(g_nfork + 1) * sizeof *g_fork);
            ForkRes *x = &g_fork[g_nfork++];
            memcpy(x->type, te, 4); x->type[4] = 0;
            for(int k = 3; k >= 0 && x->type[k] == ' '; k--) x->type[k] = 0;   /* 'snd ' -> "snd" */
            x->id = (int16_t)be16(re); x->len = (int)be32(b + at); x->data = b + at + 4;
        }
    }
    fprintf(stderr, "application fork: %d resources\n", g_nfork);
}

static const uint8_t *fork_get(const char *type, int id, int *len){
    for(int i = 0; i < g_nfork; i++)
        if(g_fork[i].id == id && !strcmp(g_fork[i].type, type)){ *len = g_fork[i].len; return g_fork[i].data; }
    fprintf(stderr, "the application has no '%s' %d\n", type, id); exit(2);
}

/* CODE 0: above-A5 size, below-A5 size, jump-table length, jump-table offset,
 * then the table. Each 8-byte entry is `offset.w; move.w #seg,-(sp); _LoadSeg`.
 * Compiled code calls entry+2 (the thunk), so that is the address wired up. */
static uint32_t load_code0(void){
    int n; const uint8_t *c0 = fork_get("CODE", 0, &n);
    uint32_t below = be32(c0+4), jtlen = be32(c0+8), jtoff = be32(c0+12);
    memcpy(M.mem + A5_BASE + jtoff, c0 + 16, (size_t)(n - 16));
    int wired = 0;
    for(uint32_t i = 0; i*8 < jtlen; i++){
        const uint8_t *e = c0 + 16 + i*8;
        uint16_t off = be16(e), push = be16(e+2), seg = be16(e+4), trap = be16(e+6);
        if(push == 0x3F3C && trap == 0xA9F0 && pop_seg_base(seg)){
            m68k_jt_set(jtoff + i*8 + 2, pop_seg_base(seg) + off);
            wired++;
        }
    }
    fprintf(stderr, "jump table: %d of %u entries wired, globals %u bytes below A5\n",
            wired, jtlen/8, below);
    return below;
}

static void load_segments(void){
    for(int s = 1; s <= 4; s++){
        int n; const uint8_t *b = fork_get("CODE", s, &n);
        if(n - 4 > SEG_SPAN){ fprintf(stderr, "CODE %d too big\n", s); exit(2); }
        memcpy(M.mem + pop_seg_base(s), b + 4, (size_t)(n - 4));   /* drop the 4-byte header */
    }
    register_seg_1(pop_seg_base(1)); register_seg_2(pop_seg_base(2));
    register_seg_3(pop_seg_base(3)); register_seg_4(pop_seg_base(4));
}

/* Every other resource, for the Resource Manager HAL (which keeps pointers
 * into the fork buffer). */
static void load_resources(void){
    int nres = 0;
    for(int i = 0; i < g_nfork; i++){
        ForkRes *r = &g_fork[i];
        if(!strcmp(r->type, "CODE")) continue;
        res_add(r->type, r->id, r->data, r->len);
        pop_resource_loaded(r->type, r->id, r->data, r->len);
        nres++;
    }
    fprintf(stderr, "loaded %d resources\n", nres);
}

/* The files on the three disks (work/files/files.txt), served read-only by the
 * File Manager HAL. The game opens its graphics set (Persia(BW), (COLOR) or
 * (LC)) by name. */
static void load_files(void){
    long n; uint8_t *t = must("files/files.txt", &n);
    char *s = (char*)realloc(t, (size_t)n + 1); s[n] = 0;
    int nf = 0;
    for(char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")){
        if(line[0] == '#') continue;
        char *f[7]; int k = 0;
        for(char *p = line; k < 7; k++){ f[k] = p; p = strchr(p, '\t'); if(!p){ k++; break; } *p++ = 0; }
        if(k < 7) continue;
        char *cr = strchr(f[6], '\r'); if(cr) *cr = 0;
        static char paths[64][2][1024];
        if(nf >= 64) break;
        const char *dp = NULL, *rp = NULL;
        if(strcmp(f[3], "-")){ snprintf(paths[nf][0], 1024, "%s/%s", g_work, f[3]); dp = paths[nf][0]; }
        if(strcmp(f[5], "-")){ snprintf(paths[nf][1], 1024, "%s/%s", g_work, f[5]); rp = paths[nf][1]; }
        fs_add(f[0], f[1], f[2], dp, (uint32_t)atol(f[4]), rp, (uint32_t)atol(f[6]));
        fprintf(stderr, "file: %-20s %s/%s data %s rsrc %s\n", f[0], f[1], f[2], f[4], f[6]);
        nf++;
    }
    /* s is intentionally kept: fs_add may hold the name pointers */
}

#ifdef POP_STANDALONE
/* ---- the single-file build ----
 * The game files (from your own disks, packed by tools/embed.py) are inside
 * the executable. The File Manager HAL reads files from disk, so on start-up
 * they are unpacked, once, and run from there.
 *
 * Where everything lives depends on the system:
 *
 * Windows -- portable: everything next to the exe,
 *     PrinceOfPersia.exe
 *     data\          the unpacked game files (data\files\...)
 *     saves\         savestates
 *     pop1.cfg       settings
 *     pop1.log       the log (there is no console)
 * so the folder can be copied or moved as a whole. Only if that folder cannot
 * be written to (say, Program Files) does it fall back to the per-user folder
 * SDL picks (%APPDATA%\pop1-recomp\PrinceOfPersia\), which is where earlier
 * versions kept everything; settings and saves found there are copied over
 * the first time the portable folder is used.
 *
 * Linux and macOS -- always the per-user folder SDL picks, with work/, saves/,
 * pop1.cfg and pop1.log inside:
 *     Linux  ~/.local/share/pop1-recomp/PrinceOfPersia/  (or $XDG_DATA_HOME)
 *     macOS  ~/Library/Application Support/pop1-recomp/PrinceOfPersia/
 * Programs there usually sit somewhere the player should not write: a
 * read-only AppImage, /usr/games, or inside a .app bundle (which writing
 * into would break). Each user also gets their own saves that way. */
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif
typedef struct { const char *path; const uint8_t *data; unsigned len; } PopEmbedded;
extern const PopEmbedded pop_embedded[];
extern const int pop_n_embedded;
char *SDL_GetPrefPath(const char *org, const char *app);    /* SDL.h would rename main */
char *SDL_GetBasePath(void);

static void mkdirs(const char *path){                /* every folder above the file */
    char p[1200]; snprintf(p, sizeof p, "%s", path);
    for(char *c = p + 1; *c; c++) if(*c == '/' || *c == '\\'){ char k = *c; *c = 0; MKDIR(p); *c = k; }
}
static int exists(const char *p){ struct stat st; return stat(p, &st) == 0; }
static void copy_file(const char *from, const char *to){
    FILE *a = fopen(from, "rb"); if(!a) return;
    FILE *b = fopen(to, "wb"); if(!b){ fclose(a); return; }
    char buf[65536]; size_t n;
    while((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a); fclose(b);
}
/* The folder to keep everything in (with a trailing separator). Windows: next
 * to the exe if a file can be written there, else the per-user folder. Linux
 * and macOS: the per-user folder. */
static const char *home_dir(int *portable){
    static char dir[1100];
#ifdef _WIN32
    char *base = SDL_GetBasePath();
    if(base){
        snprintf(dir, sizeof dir, "%s", base);
        char probe[1200]; snprintf(probe, sizeof probe, "%spop1.log", dir);
        FILE *f = fopen(probe, "ab");
        if(f){ fclose(f); *portable = 1; return dir; }
    }
#endif
    *portable = 0;
    char *pref = SDL_GetPrefPath("pop1-recomp", "PrinceOfPersia");
    if(!pref) return NULL;
    snprintf(dir, sizeof dir, "%s", pref);
    return dir;
}
static const char *unpack_embedded(void){
    int portable;
    const char *home = home_dir(&portable);
    if(!home) return NULL;
    /* settings and saves an earlier version left in the per-user folder */
    if(portable){
        char *pref = SDL_GetPrefPath("pop1-recomp", "PrinceOfPersia");
        if(pref){
            char from[1300], to[1300];
            snprintf(to, sizeof to, "%spop1.cfg", home); snprintf(from, sizeof from, "%spop1.cfg", pref);
            if(!exists(to) && exists(from)) copy_file(from, to);
            for(int i = 1; i <= 9; i++){
                snprintf(to, sizeof to, "%ssaves/slot%d.state", home, i);
                snprintf(from, sizeof from, "%ssaves/slot%d.state", pref, i);
                if(!exists(to) && exists(from)){ mkdirs(to); copy_file(from, to); }
            }
        }
    }
    static char log[1200]; snprintf(log, sizeof log, "%spop1.log", home);
#ifndef _WIN32
    /* Started from a terminal, say once where things went: the rest of the
     * log goes into the file. */
    fprintf(stderr, "Prince of Persia: saves, settings and log in %s\n", home);
#endif
    if(!freopen(log, "w", stderr)) return NULL;   /* nowhere to write */
    fprintf(stderr, "%s folder: %s\n", portable ? "portable" : "per-user", home);
    static char work[1200]; snprintf(work, sizeof work, "%s%s", home, portable ? "data" : "work");
    for(int i = 0; i < pop_n_embedded; i++){
        char path[1300]; snprintf(path, sizeof path, "%s/%s", work, pop_embedded[i].path);
        struct stat st;
        if(stat(path, &st) == 0 && (unsigned)st.st_size == pop_embedded[i].len) continue;
        mkdirs(path);
        FILE *f = fopen(path, "wb");
        if(!f){ fprintf(stderr, "cannot write %s\n", path); return NULL; }
        fwrite(pop_embedded[i].data, 1, pop_embedded[i].len, f); fclose(f);
    }
    return work;
}
#endif

/* ---- starting again ----
 * The game picks its graphics set once, at start-up, from the machine it
 * finds; a different mode is a different machine. So the menu's Graphics
 * choice starts the program afresh, with the same arguments. */
/* The new run must not replay this one's scripted keys (POPKEYS) or force
 * the old mode (POPMODE): the player has just chosen another. */
static void restart_env(void){ putenv((char*)"POPKEYS="); putenv((char*)"POPMODE="); }
#ifdef _WIN32
#include <windows.h>
void pop_restart(void){
    restart_env();
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pi, 0, sizeof pi);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    if(CreateProcessW(exe, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    else fprintf(stderr, "restart failed (%lu)\n", (unsigned long)GetLastError());
    exit(0);
}
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>                  /* _NSGetExecutablePath */
#endif
static char **g_argv;
void pop_restart(void){
    restart_env();
    fflush(NULL);
#ifdef __APPLE__
    /* macOS has no /proc: ask for the program's own path (inside the .app) */
    char self[4096]; uint32_t n = sizeof self;
    if(_NSGetExecutablePath(self, &n) == 0) execv(self, g_argv);
#else
    execv("/proc/self/exe", g_argv);
#endif
    execvp(g_argv[0], g_argv);
    perror("restart"); exit(1);
}
#endif

int main(int argc, char **argv){
    int scale = 2;
#ifndef _WIN32
    g_argv = argv;
#endif
#ifdef POP_STANDALONE
    {   const char *w = unpack_embedded();
        if(w) g_work = w; }
#endif
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--scale") && i+1 < argc) scale = atoi(argv[++i]);
        else if(argv[i][0] == '-'){ fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else g_work = argv[i];
    }
    /* Double-clicked, the program starts in whatever folder the shell picks.
     * If work/ is not here, look next to the executable and one level up
     * (build/pop1.exe in the repo). */
    if(!strcmp(g_work, "work")){            /* (the standalone build has set its own) */
        char probe[1100]; FILE *f;
        snprintf(probe, sizeof probe, "%s/files/files.txt", g_work);
        if(!(f = fopen(probe, "rb"))){
            static char dir[1024]; snprintf(dir, sizeof dir, "%s", argv[0]);
            char *slash = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
            if(bs > slash) slash = bs;
            if(slash){
                *slash = 0;
                static char cand[2][1100];
                snprintf(cand[0], sizeof cand[0], "%s/work", dir);
                snprintf(cand[1], sizeof cand[1], "%s/../work", dir);
                for(int k = 0; k < 2; k++){
                    snprintf(probe, sizeof probe, "%s/files/files.txt", cand[k]);
                    if((f = fopen(probe, "rb"))){ fclose(f); g_work = cand[k]; break; }
                }
            }
        } else fclose(f);
    }

    M.memsize = MEM_SIZE;
    M.mem = (uint8_t*)calloc(1, MEM_SIZE);
    if(!M.mem){ perror("mem"); return 1; }

    load_fork();
    load_segments();
    uint32_t below = load_code0();

    M.a[5] = A5_BASE;               /* before toolbox_init: it records CurrentA5 */
    SP     = STACK_TOP;
    toolbox_init();
    m68k_w32(LM_CURRENTA5, A5_BASE);
    if(!getenv("MRAPP")) putenv("MRAPP=Prince of Persia");
    pop_init();

    load_resources();
    load_files();
    if(plat_open("Prince of Persia", scale)){ fprintf(stderr, "no display\n"); return 1; }

    /* THINK C startup, CODE 1 offset 0: `clr.w $a4a; suba.l a6,a6`, then the
     * globals initialiser at 0x30 (fills CurStackBase..A5 from DATA/ZERO,
     * applies DREL), then `jsr (a5,d1.w)` with d1 = the long at CODE 1 + 0,
     * which is main's jump-table slot. The lifter does not take that indexed
     * jsr, so the last step is done here. CurStackBase is what the Segment
     * Loader would have set: A5 minus the globals' size. */
    m68k_w32(LM_CURSTACKBASE, A5_BASE - below);
    m68k_w16(0x0a4a, 0);
    M.a[6] = 0;
    m68k_call(pop_seg_base(1) + 0x30);
    uint32_t main_slot = m68k_r32(pop_seg_base(1));
    fprintf(stderr, "boot: calling main via A5+%x...\n", main_slot);
    m68k_jt_call(main_slot);
    fprintf(stderr, "boot: main returned\n");

    plat_present();
    plat_shutdown();
    return 0;
}
