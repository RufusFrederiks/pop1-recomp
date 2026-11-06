/* state.c - savestates.
 *
 * A static recompilation keeps part of the machine on the native stack: a
 * lifted function that is part-way through is a C function part-way through.
 * Such a stack cannot be written to a file. So a state is taken, and put
 * back, at one point only, where that stack is always the same: the game's
 * single WaitNextEvent call, which main (CODE 2 offset 0) makes itself at the
 * top of its loop, once per frame, at a lifted call depth of one (checked:
 * the call stack there is main and nothing else, from the title to the end of
 * the game). A request (menu, F5/F9, POPKEYS save1@t / load1@t) waits for the
 * next pass there; at that moment everything below main's frame is idle and
 * the whole machine is data:
 *
 *  - the 68k registers and every non-zero 4 KB page of guest memory (the
 *    game's globals, its heap and stack, the screens, the colour tables and
 *    palettes, the VBL queue, the Sound Manager's channels);
 *  - the host-side state of the Toolbox: macrecomp's window list, heap
 *    pointer, resource handles and handle sizes, QuickDraw's pen and clip, the
 *    open files (src/mr/*_state.c compile those files unchanged so their
 *    statics are in reach), and this recomp's own (regions, detached
 *    resources, where the music drivers live, what is playing, the device);
 *  - the clock: Ticks carries on from the saved value.
 *
 * Loading reads the whole file first and checks it; the current machine is
 * kept in memory and put back if anything in the file does not fit, so a bad
 * or old file never leaves a half-loaded game. Files live in saves/ next to
 * pop1.cfg, one per slot (1-9). A state belongs to its graphics mode: the
 * machine (screen, heap layout) differs between colour and B&W. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include "mr/state_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

#define MAGIC "POP1STAT"
#define VERSION 2               /* 2: + the heap's free lists (version 1 still loads) */
#define PAGE 4096u

/* ---- the stream (a growable buffer) ---- */
typedef struct { uint8_t *b; size_t len, cap, pos; } Buf;
static Buf g_buf;              /* the stream st_bytes works on */

static void buf_put(Buf *x, const void *p, size_t n){
    if(x->len + n > x->cap){
        size_t c = x->cap ? x->cap : 1 << 20;
        while(c < x->len + n) c *= 2;
        uint8_t *nb = (uint8_t*)realloc(x->b, c);
        if(!nb){ fprintf(stderr, "[state] out of memory\n"); exit(1); }
        x->b = nb; x->cap = c;
    }
    memcpy(x->b + x->len, p, n); x->len += n;
}
static int buf_get(Buf *x, void *p, size_t n){
    if(x->pos + n > x->len) return 0;
    memcpy(p, x->b + x->pos, n); x->pos += n; return 1;
}

void st_bytes(StateIO *io, void *p, size_t n){
    if(io->err) return;
    uint32_t len = (uint32_t)n;
    if(io->save){ buf_put(&g_buf, &len, 4); buf_put(&g_buf, p, n); return; }
    uint32_t got;
    if(!buf_get(&g_buf, &got, 4) || got != len){ io->err = 1; return; }
    if(!buf_get(&g_buf, p, n)) io->err = 1;
}

/* ---- the header ---- */
typedef struct {
    char magic[8];
    uint32_t version, mode, ticks, depth, main_fn;
    int64_t when;                       /* time(), for the menu */
} Header;

/* ---- the whole machine, one way or the other ---- */
static uint32_t g_load_version = VERSION;   /* the version of the file being loaded */
static void machine(StateIO *io){
    ST(io, M.d); ST(io, M.a); ST(io, M.pc);
    ST(io, M.x); ST(io, M.n); ST(io, M.z); ST(io, M.v); ST(io, M.c);
    mrs_toolbox(io); mrs_quickdraw(io); mrs_files(io); mrs_dialog(io);
    pop_traps_state(io); pop_qd_state(io);
    { int n; void *g = pop_cqd_state(&n); st_bytes(io, g, (size_t)n); }
    pop_sound_state(io);
    if(io->save || g_load_version >= 2) pop_heap_state(io);
    else pop_heap_reset();
    /* guest memory: the pages that are not all zero */
    if(io->err) return;
    uint32_t npages = M.memsize / PAGE; ST(io, npages);
    if(io->save){
        static const uint8_t zero[PAGE];
        for(uint32_t i = 0; i < npages; i++){
            if(!memcmp(M.mem + i * PAGE, zero, PAGE)) continue;
            buf_put(&g_buf, &i, 4); buf_put(&g_buf, M.mem + i * PAGE, PAGE);
        }
        uint32_t end = 0xFFFFFFFFu; buf_put(&g_buf, &end, 4);
    } else {
        if(npages != M.memsize / PAGE){ io->err = 1; return; }
        /* check the page list before touching memory */
        size_t at = g_buf.pos; uint32_t i;
        for(;;){
            if(!buf_get(&g_buf, &i, 4)){ io->err = 1; return; }
            if(i == 0xFFFFFFFFu) break;
            if(i >= npages || g_buf.pos + PAGE > g_buf.len){ io->err = 1; return; }
            g_buf.pos += PAGE;
        }
        g_buf.pos = at;
        memset(M.mem, 0, M.memsize);
        for(;;){
            buf_get(&g_buf, &i, 4);
            if(i == 0xFFFFFFFFu) break;
            buf_get(&g_buf, M.mem + i * PAGE, PAGE);
        }
    }
}

/* ---- files ---- */
static void slot_path(int slot, char *out, size_t n){
    snprintf(out, n, "%s/../saves/slot%d.state", pop_work_dir(), slot);
}
static void ensure_dir(void){
    char p[1200]; snprintf(p, sizeof p, "%s/../saves", pop_work_dir()); MKDIR(p);
}

/* What is in a slot, for the menu: 0 empty, else the time it was saved. */
int pop_state_info(int slot, char *out, size_t n){
    char p[1200]; slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if(!f){ snprintf(out, n, "EMPTY"); return 0; }
    Header h; size_t got = fread(&h, 1, sizeof h, f); fclose(f);
    if(got != sizeof h || memcmp(h.magic, MAGIC, 8)){ snprintf(out, n, "NOT A STATE"); return 0; }
    static const char *MODES[] = { "B&W", "COLOUR", "LC" };
    time_t t = (time_t)h.when; struct tm *tm = localtime(&t);
    char when[64] = "";
    if(tm) strftime(when, sizeof when, "%d %b %H:%M", tm);
    snprintf(out, n, "%s, %s%s", when, h.mode <= 2 ? MODES[h.mode] : "?",
             h.version > VERSION ? " (NEWER)" : "");
    return 1;
}

static int do_save(int slot){
    g_buf.len = 0;
    Header h; memset(&h, 0, sizeof h);
    memcpy(h.magic, MAGIC, 8); h.version = VERSION; h.mode = (uint32_t)pop_colour_mode();
    h.ticks = m68k_r32(0x16A); h.depth = (uint32_t)g_shadow_sp; h.main_fn = g_shadow[0];
    h.when = (int64_t)time(NULL);
    buf_put(&g_buf, &h, sizeof h);
    StateIO io = { NULL, 1, 0 };
    machine(&io);
    ensure_dir();
    char p[1200], tmp[1300]; slot_path(slot, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if(!f){ fprintf(stderr, "[state] cannot write %s\n", tmp); return 0; }
    int ok = fwrite(g_buf.b, 1, g_buf.len, f) == g_buf.len;
    ok = (fclose(f) == 0) && ok;
    remove(p);
    if(!ok || rename(tmp, p) != 0){ fprintf(stderr, "[state] cannot write %s\n", p); return 0; }
    fprintf(stderr, "[state] saved slot %d (%zu KB) at tick %u\n", slot, g_buf.len / 1024, h.ticks);
    return 1;
}

static int load_from_buf(void){
    g_buf.pos = sizeof(Header);
    StateIO io = { NULL, 0, 0 };
    machine(&io);
    return !io.err;
}

/* 1 loaded, 0 nothing changed (with a reason for the player). */
static int do_load(int slot, char *why, size_t nwhy){
    char p[1200]; slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if(!f){ snprintf(why, nwhy, "Slot %d is empty", slot); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    Buf file = { 0, 0, 0, 0 };
    if(n > 0){ file.b = (uint8_t*)malloc((size_t)n); file.cap = (size_t)n; }
    if(!file.b || fread(file.b, 1, (size_t)n, f) != (size_t)n){ fclose(f); free(file.b);
        snprintf(why, nwhy, "Cannot read slot %d", slot); return 0; }
    fclose(f); file.len = (size_t)n;
    Header h;
    if(file.len < sizeof h){ free(file.b); snprintf(why, nwhy, "Slot %d is not a state", slot); return 0; }
    memcpy(&h, file.b, sizeof h);
    if(memcmp(h.magic, MAGIC, 8) || h.version < 1 || h.version > VERSION){
        free(file.b); snprintf(why, nwhy, "Slot %d is from another version", slot); return 0; }
    if(h.mode != (uint32_t)pop_colour_mode()){
        static const char *MODES[] = { "B&W", "Colour", "LC Colour" };
        free(file.b); snprintf(why, nwhy, "Slot %d was saved in %s", slot, h.mode <= 2 ? MODES[h.mode] : "?"); return 0; }
    if(h.depth != (uint32_t)g_shadow_sp || h.main_fn != g_shadow[0]){
        free(file.b); snprintf(why, nwhy, "Slot %d: not here", slot); return 0; }

    /* keep the machine as it is, in case the file does not fit */
    Buf mine = g_buf; memset(&g_buf, 0, sizeof g_buf);
    { StateIO io = { NULL, 1, 0 }; machine(&io); }
    Buf backup = g_buf;
    g_buf = file;
    g_load_version = h.version;
    int ok = load_from_buf();
    g_load_version = VERSION;
    if(!ok){
        fprintf(stderr, "[state] slot %d does not fit this build; nothing changed\n", slot);
        g_buf = backup; load_from_buf();
        snprintf(why, nwhy, "Slot %d does not fit this version", slot);
    }
    free(file.b); free(backup.b);
    g_buf = mine;
    if(ok){
        pop_set_ticks(h.ticks);
        mrs_repair_port();
        fprintf(stderr, "[state] loaded slot %d (tick %u)\n", slot, h.ticks);
    }
    return ok;
}

/* ---- requests ---- */
static int g_req, g_slot = 1;
void pop_state_request(int what, int slot){ g_req = what; if(slot >= 1 && slot <= 9) g_slot = slot; }

/* At the top of the game's loop (traps.c, the WaitNextEvent from main). */
void pop_state_poll(void){
    if(!g_req) return;
    /* not in the middle of a room change (play area blanked, new room owed):
     * wait a frame or two, so a state never starts on a black screen */
    static int waited;
    if(g_req == 1 && pop_update_owed() && waited < 30){ waited++; return; }
    waited = 0;
    int what = g_req; g_req = 0;
    char msg[96];
    if(what == 1){
        if(do_save(g_slot)) snprintf(msg, sizeof msg, "State saved to slot %d", g_slot);
        else snprintf(msg, sizeof msg, "Could not save slot %d", g_slot);
    } else {
        if(do_load(g_slot, msg, sizeof msg)){
            snprintf(msg, sizeof msg, "State loaded from slot %d", g_slot);
            pop_note_new_game();               /* the game is running: the menu's Resume */
            pop_flush_keys();
        }
    }
    pop_toast(msg);
}
