/* qd.c - the parts of QuickDraw and the Window Manager that Prince of Persia
 * needs more exactly than macrecomp's HAL provides.
 *
 * Three things, all about what gets redrawn and how:
 *
 *  1. Regions. macrecomp keeps a region as its bounding box. PoP builds its
 *     "border" as DiffRgn(window, play area) -- a frame -- and paints it black
 *     on every update; as a box, that paints the whole window. Here every
 *     region the game builds also has a shadow: a short list of rectangles,
 *     maintained through NewRgn/RectRgn/DiffRgn/UnionRgn/SectRgn/CopyRgn and
 *     used by PaintRgn/EraseRgn/InvertRgn. The bounding box in guest memory is
 *     kept up to date too, so everything else still sees a sensible region.
 *
 *  2. Updates. macrecomp turns any InvalRect into "the whole window is dirty"
 *     and does not clip drawing during an update. PoP's engine invalidates
 *     small rectangles (the status bar, the play area) and its update handler
 *     paints the border and recopies the whole play area, relying on
 *     BeginUpdate to clip all of that to what was invalidated. Here InvalRect
 *     accumulates the real rectangle, and between BeginUpdate and EndUpdate
 *     QuickDraw's clip is narrowed to it.
 *
 *  3. CopyBits. macrecomp treats mode $24 as "invert" and ignores the clip.
 *     In QuickDraw notSrcCopy is 4 ($24 is the colour mode `transparent`), and
 *     PoP's hurt flash copies the frame with notSrcCopy between two InverRects;
 *     copied the normal way, the room was left inverted. Here CopyBits does all
 *     eight 1-bit transfer modes and clips to the port when it draws into it.
 *
 * Coordinates follow macrecomp's convention for the screen (a port's local
 * coordinates are used as screen coordinates; the bounds origin is applied
 * to offscreen bitmaps only), so everything here lines up with what the rest
 * of the HAL draws. Semantics from Inside Macintosh I (QuickDraw, Window
 * Manager). */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include "mr/state_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t pop32(void){ uint32_t v = m68k_r32(SP); SP += 4; return v; }
static uint16_t pop16(void){ uint16_t v = (uint16_t)m68k_r16(SP); SP += 2; return v; }
static void push32(uint32_t v){ SP -= 4; m68k_w32(SP, v); }

static Rect rd_rect(uint32_t p){
    Rect r; r.top = (int16_t)m68k_r16(p); r.left = (int16_t)m68k_r16(p+2);
    r.bottom = (int16_t)m68k_r16(p+4); r.right = (int16_t)m68k_r16(p+6); return r; }
static void wr_rect(uint32_t p, const Rect *r){
    m68k_w16(p, (uint16_t)r->top); m68k_w16(p+2, (uint16_t)r->left);
    m68k_w16(p+4, (uint16_t)r->bottom); m68k_w16(p+6, (uint16_t)r->right); }
static int empty(const Rect *r){ return r->right <= r->left || r->bottom <= r->top; }
static int sect(const Rect *a, const Rect *b, Rect *o){ return rect_sect(a, b, o); }

/* ---- 1. regions as rectangle lists ---- */
#define MAXR 24
typedef struct { uint32_t h; int n; Rect r[MAXR]; } Shadow;
#define MAXS 64
static Shadow g_sh[MAXS];

static Shadow *sh_find(uint32_t h){
    if(!h) return NULL;
    for(int i = 0; i < MAXS; i++) if(g_sh[i].h == h) return &g_sh[i];
    return NULL;
}
static Shadow *sh_get(uint32_t h){
    Shadow *s = sh_find(h);
    if(s || !h) return s;
    for(int i = 0; i < MAXS; i++) if(!g_sh[i].h){ s = &g_sh[i]; s->h = h; s->n = 0; return s; }
    return NULL;                              /* full: the region stays a box */
}
/* A region's rectangles: the shadow if there is one, else its bounding box. */
static int rgn_rects(uint32_t h, Rect *out){
    Shadow *s = sh_find(h);
    if(s){ memcpy(out, s->r, sizeof(Rect) * (size_t)s->n); return s->n; }
    if(!h || !m68k_r32(h)) return 0;
    out[0] = rd_rect(m68k_r32(h) + 2);
    return empty(&out[0]) ? 0 : 1;
}
/* Store a result: the shadow, and the bounding box in guest memory. */
static void rgn_set(uint32_t h, const Rect *r, int n){
    if(!h) return;
    Shadow *s = sh_get(h);
    Rect bb = {0,0,0,0};
    int k = 0;
    for(int i = 0; i < n; i++){
        if(empty(&r[i])) continue;
        if(k == 0) bb = r[i]; else rect_union(&bb, &r[i], &bb);
        if(s && k < MAXR) s->r[k] = r[i];
        k++;
    }
    if(s) s->n = k < MAXR ? k : MAXR;
    uint32_t p = m68k_r32(h);
    if(p){ m68k_w16(p, 10); wr_rect(p + 2, &bb); }
}
/* a minus b: up to four pieces. */
static int rect_minus(const Rect *a, const Rect *b, Rect *out){
    Rect i;
    if(!sect(a, b, &i)){ out[0] = *a; return 1; }
    int n = 0;
    if(a->top < i.top){ out[n] = *a; out[n].bottom = i.top; n++; }
    if(i.bottom < a->bottom){ out[n] = *a; out[n].top = i.bottom; n++; }
    if(a->left < i.left){ out[n] = i; out[n].left = a->left; out[n].right = i.left; n++; }
    if(i.right < a->right){ out[n] = i; out[n].left = i.right; out[n].right = a->right; n++; }
    return n;
}
static int list_minus(const Rect *a, int na, const Rect *b, int nb, Rect *out){
    Rect cur[MAXR * 4], nxt[MAXR * 4]; int nc = 0;
    for(int i = 0; i < na && nc < MAXR * 4; i++) cur[nc++] = a[i];
    for(int j = 0; j < nb; j++){
        int nn = 0;
        for(int i = 0; i < nc; i++){
            Rect piece[4]; int k = rect_minus(&cur[i], &b[j], piece);
            for(int q = 0; q < k && nn < MAXR * 4; q++) nxt[nn++] = piece[q];
        }
        memcpy(cur, nxt, sizeof(Rect) * (size_t)nn); nc = nn;
    }
    if(nc > MAXR) nc = MAXR;
    memcpy(out, cur, sizeof(Rect) * (size_t)nc);
    return nc;
}

/* Paint a region's rectangles: mode 0 paint (pen), 1 erase, 2 invert. In a
 * colour port, cqd.c fills them with the port's colours. */
static void rgn_draw(uint32_t h, int mode){
    Rect r[MAXR]; int n = rgn_rects(h, r);
    for(int i = 0; i < n; i++){
        if(pop_cqd_fill(&r[i], mode)) continue;
        if(mode == 0) qd_paint_rect(&r[i]);
        else if(mode == 1) qd_erase_rect(&r[i]);
        else qd_invert_rect(&r[i]);
    }
}

/* A new region holding one rectangle (for cqd.c's ports), and setting one. */
uint32_t pop_qd_rect_rgn(const Rect *r){
    uint32_t d0 = M.d[0], a0 = M.a[0];
    SP -= 4; m68k_w32(SP, 0);
    mr_toolbox_trap(0xA8D8);                  /* NewRgn */
    uint32_t h = pop32();
    M.d[0] = d0; M.a[0] = a0;
    Shadow *s = sh_get(h); if(s) s->n = 0;
    rgn_set(h, r, 1);
    return h;
}
void pop_qd_set_rect_rgn(uint32_t h, int t, int l, int b, int r){
    Rect x = { (int16_t)t, (int16_t)l, (int16_t)b, (int16_t)r };
    rgn_set(h, &x, 1);
}

/* ---- 2. update regions ---- */
#define WR_UPDATERGN 122
static Rect g_saved_clip;
static int g_in_update;
/* An InvalRect not yet answered by an update: the screen is mid-change (at a
 * room change the engine blanks the play area and the new room arrives with
 * the update, one game frame later). The front-end holds the previous frame
 * until then. */
static int g_update_owed;
int pop_update_owed(void){ return g_update_owed; }

/* The window's update region bounding box, as macrecomp stores it. */
static Rect upd_get(uint32_t w){
    Rect z = {0,0,0,0};
    uint32_t h = w ? m68k_r32(w + WR_UPDATERGN) : 0;
    if(!h || !m68k_r32(h)) return z;
    return rd_rect(m68k_r32(h) + 2);
}
static void upd_set(uint32_t w, const Rect *r){
    uint32_t h = w ? m68k_r32(w + WR_UPDATERGN) : 0;
    if(h && m68k_r32(h)) wr_rect(m68k_r32(h) + 2, r);
}
static uint32_t current_port(void){
    uint32_t slot = SP - 64;
    push32(slot); mr_toolbox_trap(0xA874);   /* GetPort */
    return m68k_r32(slot);
}

/* ---- 3. CopyBits ---- */
typedef struct { uint32_t base; int rb; Rect b; int screen; } Bits;
static Bits bits(uint32_t bm){
    Bits x; x.base = m68k_r32(bm); x.rb = (int)(m68k_r16(bm + 4) & 0x3FFF); x.b = rd_rect(bm + 6);
    x.screen = (x.base == mr_screen_base() || x.base == 0);
    return x;
}
static int bit_get(const Bits *s, int x, int y){
    if(s->screen) return qd_screen_get(x, y);
    int lx = x - s->b.left, ly = y - s->b.top;
    if(lx < 0 || ly < 0 || x >= s->b.right || y >= s->b.bottom) return 0;
    uint32_t a = s->base + (uint32_t)ly * (uint32_t)s->rb + (uint32_t)(lx >> 3);
    return a < M.memsize ? (M.mem[a] >> (7 - (lx & 7))) & 1 : 0;
}
static void bit_put(const Bits *d, int x, int y, int v){
    if(d->screen){ qd_screen_put(x, y, v); return; }
    int lx = x - d->b.left, ly = y - d->b.top;
    if(lx < 0 || ly < 0 || x >= d->b.right || y >= d->b.bottom) return;
    uint32_t a = d->base + (uint32_t)ly * (uint32_t)d->rb + (uint32_t)(lx >> 3);
    if(a >= M.memsize) return;
    uint8_t m = (uint8_t)(0x80u >> (lx & 7));
    M.mem[a] = v ? (uint8_t)(M.mem[a] | m) : (uint8_t)(M.mem[a] & ~m);
}

static void copybits(void){
    uint32_t mask = pop32(); int mode = (int16_t)pop16();
    uint32_t drp = pop32(), srp = pop32(), dstB = pop32(), srcB = pop32();
    Rect sr = rd_rect(srp), dr = rd_rect(drp);
    /* A PixMap on either side (rowBytes bit 15): colour, cqd.c's. */
    if((m68k_r16(srcB + 4) & 0x8000) || (m68k_r16(dstB + 4) & 0x8000)){
        Rect mr[MAXR]; int nm = mask ? rgn_rects(mask, mr) : 0;
        if(pop_cqd_copybits(srcB, dstB, &sr, &dr, mode, mask ? mr : NULL, nm)) return;
    }
    Bits s = bits(srcB), d = bits(dstB);
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    if(sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    /* Clip: into the current port's own bits, QuickDraw clips to the port
     * (clipRgn, and visRgn during an update -- both are in the HAL's clip);
     * a mask region clips further. */
    Rect clip = { -32768, -32768, 32767, 32767 };
    uint32_t port = current_port();
    if(port && m68k_r32(port + 2) == m68k_r32(dstB)) qd_get_clip(&clip);
    Rect mr[MAXR]; int nm = mask ? rgn_rects(mask, mr) : 0;
    int m = mode & 7;                         /* srcCopy srcOr srcXor srcBic, then the not- forms */
    for(int y = 0; y < dh; y++){
        int sy = sr.top + y * sh / dh, py = dr.top + y;
        if(py < clip.top || py >= clip.bottom) continue;
        for(int x = 0; x < dw; x++){
            int px = dr.left + x;
            if(px < clip.left || px >= clip.right) continue;
            if(mask){
                int in = 0;
                for(int k = 0; k < nm && !in; k++) in = pt_in_rect(px, py, &mr[k]);
                if(!in) continue;
            }
            int sb = bit_get(&s, sr.left + x * sw / dw, sy);
            if(m >= 4) sb = !sb;
            switch(m & 3){
            case 0: bit_put(&d, px, py, sb); break;                              /* copy */
            case 1: if(sb) bit_put(&d, px, py, 1); break;                       /* or   */
            case 2: if(sb) bit_put(&d, px, py, !bit_get(&d, px, py)); break;    /* xor  */
            case 3: if(sb) bit_put(&d, px, py, 0); break;                       /* bic  */
            }
        }
    }
}

/* ---- the traps ---- */
int pop_qd_trap(uint16_t w, uint16_t raw){
    switch(w){
    case 0xA8D8: { /* NewRgn: RgnHandle */
        mr_toolbox_trap(raw);
        uint32_t h = m68k_r32(SP);
        Shadow *s = sh_get(h); if(s) s->n = 0;
        return 1; }
    case 0xA8D9: { /* DisposeRgn(rgn) */
        Shadow *s = sh_find(m68k_r32(SP)); if(s) s->h = 0;
        mr_toolbox_trap(raw); return 1; }
    case 0xA8DF: { /* RectRgn(rgn, r) */
        Rect r = rd_rect(pop32()); uint32_t h = pop32();
        rgn_set(h, &r, 1); return 1; }
    case 0xA8DD: { /* SetEmptyRgn(rgn) */
        rgn_set(pop32(), NULL, 0); return 1; }
    case 0xA8DC: { /* CopyRgn(src, dst) */
        uint32_t d = pop32(), s = pop32();
        Rect r[MAXR]; int n = rgn_rects(s, r); rgn_set(d, r, n); return 1; }
    case 0xA8E6: { /* DiffRgn(a, b, dst): dst = a - b */
        uint32_t d = pop32(), b = pop32(), a = pop32();
        Rect ra[MAXR], rb[MAXR], out[MAXR];
        int na = rgn_rects(a, ra), nb = rgn_rects(b, rb);
        rgn_set(d, out, list_minus(ra, na, rb, nb, out)); return 1; }
    case 0xA8E5: { /* UnionRgn(a, b, dst) = a + (b - a) */
        uint32_t d = pop32(), b = pop32(), a = pop32();
        Rect ra[MAXR], rb[MAXR], out[MAXR * 2];
        int na = rgn_rects(a, ra), nb = rgn_rects(b, rb);
        memcpy(out, ra, sizeof(Rect) * (size_t)na);
        int n = na + list_minus(rb, nb, ra, na, out + na);
        rgn_set(d, out, n > MAXR ? MAXR : n); return 1; }
    case 0xA8E4: { /* SectRgn(a, b, dst) */
        uint32_t d = pop32(), b = pop32(), a = pop32();
        Rect ra[MAXR], rb[MAXR], out[MAXR]; int n = 0;
        int na = rgn_rects(a, ra), nb = rgn_rects(b, rb);
        for(int i = 0; i < na; i++) for(int j = 0; j < nb && n < MAXR; j++)
            if(sect(&ra[i], &rb[j], &out[n])) n++;
        rgn_set(d, out, n); return 1; }
    case 0xA8D3: rgn_draw(pop32(), 0); return 1;   /* PaintRgn  */
    case 0xA8D4: rgn_draw(pop32(), 1); return 1;   /* EraseRgn  */
    case 0xA8D5: rgn_draw(pop32(), 2); return 1;   /* InvertRgn */

    case 0xA928: { /* InvalRect(r): add r to the current window's update region */
        Rect r = rd_rect(m68k_r32(SP));
        uint32_t win = current_port();
        Rect old = upd_get(win);
        int had = !empty(&old);
        mr_toolbox_trap(raw);                       /* marks the window as owing an update */
        if(getenv("POPUPD")) fprintf(stderr, "[upd] InvalRect %d,%d,%d,%d port %06x from %06x\n",
                                     r.top, r.left, r.bottom, r.right, win, g_last_call);
        g_update_owed = 1;
        if(win){ if(had) rect_union(&old, &r, &r); upd_set(win, &r); }
        return 1; }
    case 0xA922: { /* BeginUpdate(w): visRgn := updateRgn, so drawing is clipped to it */
        uint32_t win = m68k_r32(SP);
        if(getenv("POPUPD")) fprintf(stderr, "[upd] BeginUpdate %06x\n", win);
        Rect u = upd_get(win);
        mr_toolbox_trap(raw);
        qd_get_clip(&g_saved_clip);
        Rect c;
        if(!empty(&u) && sect(&g_saved_clip, &u, &c)) qd_set_clip(&c);
        else if(!empty(&u)){ Rect none = {0,0,0,0}; qd_set_clip(&none); }
        g_in_update = 1;
        return 1; }
    case 0xA923: /* EndUpdate(w) */
        mr_toolbox_trap(raw);
        g_update_owed = 0;
        if(g_in_update){ qd_set_clip(&g_saved_clip); g_in_update = 0; }
        return 1;

    case 0xA8EC: copybits(); return 1;
    }
    return 0;
}

/* Savestates: the region shadows and the update state. */
void pop_qd_state(StateIO *io){
    ST(io, g_sh); ST(io, g_saved_clip); ST(io, g_in_update); ST(io, g_update_owed);
}
