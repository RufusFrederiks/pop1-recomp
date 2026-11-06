/* traps.c - Prince of Persia's layer in front of macrecomp's Toolbox HAL.
 *
 * macrecomp's toolbox.c is compiled with -Dm68k_trap=mr_toolbox_trap, so the
 * lifted code's m68k_trap() lands here first. Traps this game needs and the
 * generic HAL does not have are handled here; everything else is forwarded.
 * The same trick puts the VBL Manager in front of the runtime's time slice.
 * Nothing in ext/macrecomp is modified.
 *
 * Trap semantics follow Inside Macintosh. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same normalisation as toolbox.c: Toolbox traps keep 10 bits, OS traps 9
 * (the flag bits 9-10 of an OS trap are not part of its number). */
static uint16_t norm(uint16_t w){ return (w & 0x0800) ? (0xA800 | (w & 0x03FF)) : (0xA000 | (w & 0x01FF)); }

/* POPTRACE=<n>: log the first n trap calls (default 3000), so a game sitting
 * in its event loop cannot fill the disk. */
static long g_trace_left = -1;
static int trace(void){
    if(g_trace_left < 0){ const char *e = getenv("POPTRACE");
        g_trace_left = e ? (atol(e) > 1 ? atol(e) : 3000) : 0; }
    /* POPTRACEFROM=<seconds>: start tracing only then. */
    static double from = -1;
    if(from < 0){ const char *e = getenv("POPTRACEFROM"); from = e ? atof(e) : 0; }
    if(from > 0 && plat_ticks() < from * 60) return 0;
    return g_trace_left > 0 && g_trace_left--;
}

static uint32_t pop32(void){ uint32_t v = m68k_r32(SP); SP += 4; return v; }
static uint16_t pop16(void){ uint16_t v = (uint16_t)m68k_r16(SP); SP += 2; return v; }
static void push32(uint32_t v){ SP -= 4; m68k_w32(SP, v); }
static void push16(uint16_t v){ SP -= 2; m68k_w16(SP, v); }

static void offset_rect(uint32_t r, int dh, int dv){
    m68k_w16(r+0, (uint16_t)((int16_t)m68k_r16(r+0) + dv));
    m68k_w16(r+2, (uint16_t)((int16_t)m68k_r16(r+2) + dh));
    m68k_w16(r+4, (uint16_t)((int16_t)m68k_r16(r+4) + dv));
    m68k_w16(r+6, (uint16_t)((int16_t)m68k_r16(r+6) + dh));
}

/* The current port, which macrecomp keeps to itself: ask it with GetPort. */
static uint32_t cur_port(void){
    uint32_t slot = SP - 64;                 /* scratch below the live stack */
    push32(slot); mr_toolbox_trap(0xA874);   /* GetPort(&slot) pops its argument */
    return m68k_r32(slot);
}

static void maybe_exit(void);

/* The 68000 interrupt mask (SR bits 8-10), kept by the lifted code for
 * `ori.w #$700,sr` / `move.w (a7)+,sr` (see tools/lift.py). The music driver
 * masks interrupts while it edits the VBL queue; a VBL task must not run
 * then. */
int pop_ipl;

/* ---- VBL Manager (VInstall / VRemove) ----
 *
 * A VBLTask is qLink(4) qType(2) vblAddr(4) vblCount(2) vblPhase(2). Every
 * tick (1/60 s) the system decrements vblCount of each installed task and,
 * when it reaches zero, calls vblAddr with A0 pointing at the task. The task
 * must set vblCount again to be called again.
 *
 * The queue is the real one, in guest low memory (VBLQueue at $160: qFlags(2)
 * qHead(4) qTail(4)), because the music driver walks it to check that its task
 * went in ("Could not locate the vbl just installed in the sys queue!") and in
 * one path links itself in by hand. Real VBL tasks run at interrupt time; here
 * they run at the next trap or loop time slice after a tick, with every
 * register saved and restored around them, which is what an interrupt looks
 * like to the code it interrupts. */
#define LM_VBLQUEUE 0x0160
#define VBL_HEAD (LM_VBLQUEUE + 2)
#define VBL_TAIL (LM_VBLQUEUE + 6)
static uint32_t g_vbl_last;
static int g_in_vbl;

static void vbl_run_due(void){
    pop_sound_poll();
    if(g_in_vbl || pop_ipl || !m68k_r32(VBL_HEAD)) return;
    uint32_t now = plat_ticks();
    if(!g_vbl_last){ g_vbl_last = now; return; }
    if(now == g_vbl_last) return;
    uint32_t ticks = now - g_vbl_last;
    if(ticks > 4) ticks = 4;                 /* after a stall, catch up a little, not all */
    g_vbl_last = now;
    g_in_vbl = 1;
    while(ticks--){
        int guard = 0;
        for(uint32_t t = m68k_r32(VBL_HEAD); t && guard < 64; guard++){
            uint32_t next = m68k_r32(t);     /* the task may remove itself */
            int16_t cnt = (int16_t)m68k_r16(t + 10);
            if(cnt > 0){
                m68k_w16(t + 10, (uint16_t)(cnt - 1));
                if(cnt == 1){
                    M68K saved = M;
                    M.a[0] = t;
                    m68k_call(m68k_r32(t + 6));
                    uint8_t *mem = M.mem; uint32_t ms = M.memsize;
                    M = saved; M.mem = mem; M.memsize = ms;
                }
            }
            t = next;
        }
    }
    g_in_vbl = 0;
}

static int vbl_install(uint32_t task){
    if(!task) return -2;                     /* qErr */
    m68k_w32(task, 0);
    m68k_w16(task + 4, 1);                   /* qType = vType */
    uint32_t tail = m68k_r32(VBL_TAIL);
    if(tail) m68k_w32(tail, task); else m68k_w32(VBL_HEAD, task);
    m68k_w32(VBL_TAIL, task);
    if(trace()) fprintf(stderr, "[pop] VInstall %06x addr %06x count %d\n",
                        task, m68k_r32(task + 6), (int16_t)m68k_r16(task + 10));
    return 0;
}
static int vbl_remove(uint32_t task){
    uint32_t prev = 0;
    for(uint32_t t = m68k_r32(VBL_HEAD); t; prev = t, t = m68k_r32(t)){
        if(t != task) continue;
        uint32_t next = m68k_r32(t);
        if(prev) m68k_w32(prev, next); else m68k_w32(VBL_HEAD, next);
        if(m68k_r32(VBL_TAIL) == t) m68k_w32(VBL_TAIL, prev);
        if(trace()) fprintf(stderr, "[pop] VRemove %06x\n", task);
        return 0;
    }
    return -1;                               /* qErr: not in the queue */
}

/* ---- resources the game changes in place ----
 *
 * macrecomp keeps one handle per resource and treats DetachResource as a
 * no-op, so a second GetResource returns the very bytes the game changed
 * after detaching them. PoP decrypts MDRV in place after DetachResource;
 * the second time it loads the driver it would decrypt it twice. On a real
 * Mac a detached handle is the caller's, and GetResource reads the resource
 * afresh. So: remember detached handles, and when GetResource hands one back,
 * give the game a new handle holding the original bytes instead. */
typedef struct { char type[5]; int id; const uint8_t *data; int len; } PopRes;
#define MAX_RES 512
static PopRes g_res[MAX_RES]; static int g_nres;
#define MAX_DET 256
static uint32_t g_detached[MAX_DET]; static int g_ndet;

void pop_resource_loaded(const char *type, int id, const uint8_t *data, int len){
    if(g_nres >= MAX_RES) return;
    PopRes *r = &g_res[g_nres++];
    snprintf(r->type, sizeof r->type, "%s", type); r->id = id; r->data = data; r->len = len;
}
static int is_detached(uint32_t h){
    for(int i = 0; i < g_ndet; i++) if(g_detached[i] == h) return 1;
    return 0;
}
static const PopRes *find_res(uint32_t ty, int id){
    char want[5] = { (char)(ty>>24), (char)(ty>>16), (char)(ty>>8), (char)ty, 0 };
    for(int n = 4; n > 0 && want[n-1] == ' '; n--) want[n-1] = 0;
    for(int i = 0; i < g_nres; i++)
        if(g_res[i].id == id && !strcmp(g_res[i].type, want)) return &g_res[i];
    return NULL;
}

/* Ticks ($16A) on the real 60 Hz clock, never going backwards. macrecomp's
 * per-call bump is compiled out (CMakeLists.txt); this is the only writer. */
static void sync_ticks(void){
    uint32_t t = plat_ticks();
    if(t > m68k_r32(0x16A)) m68k_w32(0x16A, t);
}

const uint8_t *pop_res(const char *type, int id, int *len){
    uint32_t t = 0;
    for(int i = 0; i < 4; i++) t = (t << 8) | (uint8_t)(type[i] ? type[i] : ' ');
    const PopRes *r = find_res(t, id);
    if(!r) return NULL;
    *len = r->len; return r->data;
}

static const uint8_t *menu_res(int id, int *len){
    const PopRes *r = find_res(0x4D454E55u /* 'MENU' */, id);
    if(!r) return NULL;
    *len = r->len; return r->data;
}

/* Lifted loops call this from their backward branches: every 2048 of them,
 * the same period macrecomp uses. Its own slice is not called, because all
 * it adds is one more Ticks bump. */
void m68k_time_slice(void){
    mr_tickdown = 2048;
    sync_ticks();
    maybe_exit();
    vbl_run_due();
}

/* ---- the trap entry point the lifted code calls ---- */
/* POPEXIT=<seconds>: leave cleanly after that long (headless runs, CI), so
 * exit handlers such as MRTRAPS's histogram still get to run. */
static void maybe_exit(void){
    static long secs = -1;
    if(secs < 0){ const char *e = getenv("POPEXIT"); secs = e ? atol(e) : 0; }
    if(secs && plat_ticks() >= (uint32_t)secs * 60u){
        fprintf(stderr, "[pop] POPEXIT after %ld s\n", secs);
        plat_present();
        exit(0);
    }
}

/* POPSHOTS=<s1,s2,...>: save the screen as <POPSHOTDIR or .>/shot_<s>.pgm at
 * those wall-clock seconds. For headless runs: a picture of a moment. */
static void maybe_shot(void){
    static double when[256]; static int n = -1, next;
    if(n < 0){
        n = 0; const char *e = getenv("POPSHOTS");
        if(e){ char buf[4096]; snprintf(buf, sizeof buf, "%s", e);
               for(char *t = strtok(buf, ","); t && n < 256; t = strtok(NULL, ",")) when[n++] = atof(t); }
    }
    if(next >= n || plat_ticks() < when[next] * 60) return;
    const char *dir = getenv("POPSHOTDIR");
    char path[600]; snprintf(path, sizeof path, "%s/shot_%06.2f.pgm", dir ? dir : ".", when[next]);
    FILE *f = fopen(path, "wb");
    if(f){ fprintf(f, "P5\n%d %d\n255\n", QD_W, QD_H);
           for(int y = 0; y < QD_H; y++) for(int x = 0; x < QD_W; x++) fputc(qd_screen_get(x, y) ? 0 : 255, f);
           fclose(f); fprintf(stderr, "[pop] screenshot %s\n", path); }
    next++;
}

void m68k_trap(uint16_t raw){
    maybe_exit();
    {   /* Screenshots only where the window is presented (the event calls),
         * so they show what a player sees, never a half-drawn frame. */
        uint16_t n = norm(raw);
        if((n == 0xA970 || n == 0xA860) && !pop_update_owed()) maybe_shot(); }
    sync_ticks();
    vbl_run_due();
    uint16_t w = norm(raw);
    if(pop_sound_trap(w)) return;
    if(pop_qd_trap(w, raw)) return;
    switch(w){
    case 0xA033: /* VInstall(A0 = VBLTask*) : D0 */
        M.d[0] = (uint32_t)vbl_install(M.a[0]); fl_logic(M.d[0], 2); return;
    case 0xA034: /* VRemove(A0 = VBLTask*) : D0 */
        M.d[0] = (uint32_t)vbl_remove(M.a[0]); fl_logic(M.d[0], 2); return;

    /* PurgeMem(D0 = bytes): nothing is purgeable in a bump heap. */
    case 0xA04D:
        M.d[0] = 0; fl_logic(0, 2); return;

    case 0xA992: { /* DetachResource(h) */
        uint32_t h = m68k_r32(SP);
        if(h && !is_detached(h) && g_ndet < MAX_DET) g_detached[g_ndet++] = h;
        mr_toolbox_trap(raw);
        return; }
    case 0xA9A0: case 0xA81F: { /* GetResource / Get1Resource(type, id): Handle */
        int id = (int16_t)m68k_r16(SP); uint32_t ty = m68k_r32(SP + 2);
        mr_toolbox_trap(raw);
        uint32_t h = m68k_r32(SP);
        const PopRes *r;
        if(h && is_detached(h) && (r = find_res(ty, id))){
            M.d[0] = (uint32_t)r->len; mr_toolbox_trap(0xA122);   /* NewHandle -> A0 */
            uint32_t nh = M.a[0], p = nh ? m68k_r32(nh) : 0;
            if(p){ memcpy(M.mem + p, r->data, (size_t)r->len); m68k_w32(SP, nh); }
            if(trace()) fprintf(stderr, "[pop] GetResource '%s' %d: detached before, fresh copy %06x\n",
                                r->type, id, nh);
        }
        return; }

    /* FlushEvents is a register trap: D0 = stopMask<<16 | eventMask, nothing
     * on the stack (the Pascal glue is `move.l (a7)+,d0; _FlushEvents`).
     * macrecomp 708adac pops a longword here, which leaves SP 4 bytes high;
     * in PoP's frameless init routine the following rts then lands on main's
     * caller's return address and the game quits before it starts. Upstream
     * bug, worked around here. Nothing is queued at that point, so not
     * flushing is harmless. */
    case 0xA032:
        M.d[0] = 0; fl_logic(0, 2); return;

    /* DebugStr(msg): the music driver reports its internal errors this way.
     * Print it and carry on, as MacsBug's `g` would. */
    case 0xABFF: {
        uint32_t p = pop32();
        int n = p ? (int)m68k_r8(p) : 0;
        fprintf(stderr, "[DebugStr] ");
        for(int i = 0; i < n; i++) fputc((int)m68k_r8(p + 1 + i), stderr);
        fprintf(stderr, "  (from %06x)\n", g_last_call);
        return; }

    /* WaitNextEvent(mask, VAR evt, sleep, mouseRgn): Boolean. With nothing
     * else running, it is GetNextEvent(mask, VAR evt) plus two arguments. */
    case 0xA860:
        (void)pop32(); (void)pop32();        /* mouseRgn, sleep */
        pop_frame();                         /* the top of the game's loop: show the frame */
        if(!pop_key_event(0)) mr_toolbox_trap(0xA970);
        return;
    /* GetNextEvent / EventAvail: key events are ours (src/input.c), the rest
     * is macrecomp's. */
    case 0xA970: case 0xA971:
        if(w == 0xA970) pop_frame();
        if(!pop_key_event(w == 0xA971)) mr_toolbox_trap(raw);
        return;
    /* UprString(A0 = text, D0.w = length): upper-case in place (Mac Roman
     * ASCII letters; the diacritics-stripping variant is the same here). */
    case 0xA054: {
        uint32_t p = M.a[0]; int n = (int)(M.d[0] & 0xFFFF);
        for(int i = 0; i < n; i++){ uint8_t c = (uint8_t)m68k_r8(p + (uint32_t)i);
            if(c >= 'a' && c <= 'z') m68k_w8(p + (uint32_t)i, c - 32); }
        M.d[0] = 0; fl_logic(0, 2); return; }
    /* GetPenState(VAR pnState): pnLoc(4) pnSize(4) pnMode(2) pnPat(8). The
     * HAL keeps only the location; the rest is the default pen. */
    case 0xA898: {
        uint32_t ps = pop32(); int h, v; qd_get_pen(&h, &v);
        m68k_w16(ps, (uint16_t)v); m68k_w16(ps + 2, (uint16_t)h);
        m68k_w16(ps + 4, 1); m68k_w16(ps + 6, 1); m68k_w16(ps + 8, 8);   /* 1x1, patCopy */
        for(int i = 0; i < 8; i++) m68k_w8(ps + 10 + (uint32_t)i, 0xFF);
        return; }
    /* ChangedResource(h) / WriteResource(h): the game saves its options
     * (Sounds, Music) into its own resource fork. The disk image is not
     * written back; the front-end keeps its own settings. */
    case 0xA9AA: case 0xA9B0:
        (void)pop32(); return;
    case 0xA86A: { /* HiWord(x: LONGINT): INTEGER */
        uint32_t x = pop32(); m68k_w16(SP, (uint16_t)(x >> 16)); return; }
    case 0xA86B: { /* LoWord(x: LONGINT): INTEGER */
        uint32_t x = pop32(); m68k_w16(SP, (uint16_t)x); return; }
    case 0xA976: /* GetKeys(VAR theKeys: KeyMap) */
        pop_get_keys(pop32());
        return;
    case 0xA93E: { /* MenuKey(ch): LONGINT */
        int ch = pop16() & 0xFF;
        uint32_t r = pop_take_forced_menu();
        if(!r) r = pop_menu_key(ch, menu_res);
        if(r == ((1001u << 16) | 1)) pop_note_new_game();      /* File > New Game */
        if(trace() || r) fprintf(stderr, "[pop] MenuKey '%c' -> menu %u item %u\n",
                                 ch, (unsigned)(r >> 16), (unsigned)(r & 0xFFFF));
        m68k_w32(SP, r);
        return; }

    /* ResrvMem(D0 = size): there is no heap fragmentation to plan around. */
    case 0xA040:
        M.d[0] = 0; fl_logic(0, 2); return;

    /* ForeColor / BackColor(color: LONGINT). On a 1-bit screen the game asks
     * for black on white, which is what QuickDraw does anyway.
     * ponytail: colour mode will need these for real. */
    case 0xA862: case 0xA863:
        (void)pop32(); return;

    /* SetOrigin(h, v): move the current port's local coordinates so that its
     * top-left reads (h, v). portRect, portBits.bounds and visRgn move;
     * clipRgn does not (Inside Macintosh I-166). PoP centres its play area in
     * a screen-sized window this way and then blits to portBits.baseAddr
     * itself, so the bounds have to be right. */
    case 0xA878: {
        int v = (int16_t)pop16(), h = (int16_t)pop16();
        uint32_t p = cur_port();
        if(!p) return;
        int dh = h - (int16_t)m68k_r16(p + 16 + 2), dv = v - (int16_t)m68k_r16(p + 16);
        if(!dh && !dv) return;
        offset_rect(p + 16, dh, dv);                    /* portRect */
        offset_rect(p + 2 + 6, dh, dv);                 /* portBits.bounds */
        uint32_t vis = m68k_r32(p + 24);
        if(vis && m68k_r32(vis)) offset_rect(m68k_r32(vis) + 2, dh, dv);   /* visRgn bbox */
        push32(p); mr_toolbox_trap(0xA873);             /* SetPort again: re-read the bounds */
        if(trace()) fprintf(stderr, "[pop] SetOrigin(%d,%d) port %06x\n", h, v, p);
        return; }

    /* NewWindow / NewCWindow (same arguments; colour comes later). A window
     * created visible is the front window and gets an activateEvt on a real
     * Mac. macrecomp raises one only on ShowWindow/SelectWindow, and PoP sets
     * its "running" flag (A5-0x69e5) only in its activate handler, so without
     * this it idles in the event loop for ever. SelectWindow does exactly the
     * bookkeeping wanted (front window, hilited, activate + update owed). */
    case 0xA913: case 0xAA45: {
        mr_toolbox_trap(0xA913);
        uint32_t w = m68k_r32(SP);           /* the function result */
        if(w && m68k_r8(w + 110)){ push32(w); mr_toolbox_trap(0xA91F); }
        if(trace()) fprintf(stderr, "[pop] NewWindow -> %06x%s\n", w,
                            (w && m68k_r8(w + 110)) ? " (visible: activated)" : "");
        return; }
    default: {
        /* POPTRACE=1: every forwarded trap with its stack effect. A Pascal
         * trap that pops the wrong number of bytes shows up here as the wrong
         * delta long before it shows up as a crash. */
        uint32_t sp0 = SP;
        mr_toolbox_trap(raw);
        if(w != 0xA975 && trace()) fprintf(stderr, "[pop] trap $%04X  sp %06x -> %06x (%+d)  from %06x\n",
                            raw, sp0, SP, (int)(SP - sp0), g_last_call);
        return; }
    }
}

/* ---- the music driver ----
 *
 * MDRV 11 and 22 ship encrypted and LZSS-packed; CODE 2 0x4dda unpacks one
 * into a fresh handle and returns it in D0, and the game then calls into it.
 * tools/prepare.py decodes both the same way at build time and lifts them as
 * pseudo-segments 11 and 22, whose code addresses are relative to wherever
 * the block lands. So: wrap 0x4dda, run the game's own unpacker, and register
 * the lifted driver at the address it unpacked to. The unpacked size, which
 * is the first long of the decrypted input, says which driver it is. */
void fn_2_4dda(uint32_t entry);
void register_seg_11(uint32_t base);
void register_seg_22(uint32_t base);

static void mdrv_unpack(uint32_t entry){
    uint32_t in_h = m68k_r32(SP + 4);        /* SP+0 is m68k_call's return marker */
    uint32_t size = (in_h && m68k_r32(in_h)) ? m68k_r32(m68k_r32(in_h)) : 0;
    fn_2_4dda(entry);
    uint32_t h = M.d[0], base = h ? m68k_r32(h) : 0;
    if(!base) return;
    if(size == 22174)      register_seg_11(base);
    else if(size == 22590) register_seg_22(base);
    else { fprintf(stderr, "[pop] unknown music driver (%u bytes) at %06x\n", size, base); return; }
    fprintf(stderr, "[pop] music driver MDRV %d unpacked at %06x\n", size == 22174 ? 11 : 22, base);
}

void pop_init(void){
    m68k_register(pop_seg_base(2) + 0x4dda, 0, mdrv_unpack);

    /* A real Mac's VBL queue is never empty: the system's own tasks (the
     * cursor, the disk-insert poll) are already in it. The music driver
     * relies on that. After VInstall it walks the queue for the element whose
     * link points at its task -- the predecessor -- to move its task to the
     * front, and never looks at the head itself; with an empty queue it finds
     * nothing and gives up. So the queue starts with one inert system task
     * (vblCount 0: never called). */
    uint32_t sys = mr_alloc(14);
    m68k_w32(sys, 0); m68k_w16(sys + 4, 1); m68k_w32(sys + 6, 0);
    m68k_w16(sys + 10, 0); m68k_w16(sys + 12, 0);
    m68k_w16(LM_VBLQUEUE, 0); m68k_w32(VBL_HEAD, sys); m68k_w32(VBL_TAIL, sys);
}

