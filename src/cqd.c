/* cqd.c - Colour QuickDraw, the Graphics Device Manager and the Palette
 * Manager, as far as Prince of Persia's colour mode needs them.
 *
 * How the game decides (CODE 2 0x1d8 / 0x4da, disassembly offsets): it calls
 * SysEnvirons; if hasColorQD is set it asks for the main GDevice, reads the
 * depth of its PixMap (cmpCount x cmpSize) and the size of its gdRect, and
 *   - 8 (or 4) bits deep and at least 640x400  -> Persia(COLOR), 640x400 play
 *     area (a 13" Mac II screen, 640x480);
 *   - 8 (or 4) bits deep but smaller           -> Persia(LC), the 512x334
 *     layout of the black-and-white game, in colour (the LC's 12" 512x384);
 *   - anything else                            -> Persia(BW).
 * macrecomp is a 1-bit Mac SE, so the game has always taken the last branch.
 * This file makes the machine a colour Mac when the player asks for one
 * (pop_colour_mode() != 0): an 8-bit screen in guest memory, a GDevice with a
 * PixMap and a colour table describing it, and the traps the colour path
 * calls. In black-and-white mode none of it exists and nothing here runs.
 *
 * What the game does with them, in colour:
 *  - OpenCPort for its offscreen frame buffer, whose PixMap it then points at
 *    its own NewPtr block (rowBytes | $8000); its sprite blitters (CODE 2
 *    0x3dc4 ff., 68020 code) write bytes there.
 *  - NewCWindow for the screen-sized window, SetOrigin to centre the play
 *    area, and CopyBits (PixMap -> PixMap, srcCopy) for every frame.
 *  - A palette from 'clut' 2000 (201 colours), every entry pmAnimated except
 *    0 and 1 (white, black: pmTolerant). It then asks PmForeColor(i) for each
 *    entry and reads the port's fgColor back, building its own translation
 *    from its colours to device indices (A5-0x5d06). So the index a palette
 *    entry gets is the device's business; here entry i simply gets index i.
 *  - AnimatePalette for the fades (CODE 2 0x23c4: a scaled copy of the clut,
 *    one step per tick) and for each level's 13 colours (0x24c4, entries
 *    0x69..0x75); AnimateEntry for the flashes (0x246e).
 *  - Text (the status line, the high scores) in its own font, FONT 25616
 *    ("Persian" 16), drawn into the offscreen port with TextMode srcBic.
 *
 * Everything the game can see (the GDevice, the PixMaps, the colour tables,
 * the palettes, every port field) lives in guest memory, so a savestate is
 * mostly guest memory. Semantics from Inside Macintosh V and VI (Color
 * QuickDraw, Graphics Devices, Palette Manager). Nothing is copied from
 * mac-recomp/pop2, which has no licence. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the machine ---- */
static int g_mode;                       /* 0 B&W, 1 colour 640x480, 2 LC 512x384 */
static int SW, SH, SRB;                  /* screen width, height, rowBytes */
/* The device, in guest memory. Kept in a struct so savestates can carry it. */
static struct {
    uint32_t scr;                        /* 8-bit screen */
    uint32_t gdh;                        /* GDevice handle */
    uint32_t pmh;                        /* its PixMap handle */
    uint32_t ctab;                       /* its ColorTable handle */
    uint32_t wmgr;                       /* the Window Manager's colour port */
    /* window -> palette (SetPalette) */
    uint32_t pal_win[8], pal_pal[8];
} G;

int pop_colour_mode(void){ return g_mode; }
void *pop_cqd_state(int *len){ *len = (int)sizeof G; return &G; }

/* Low-memory globals (Inside Macintosh V, VI). */
#define LM_SCRNBASE   0x0824
#define LM_SCREENROW  0x0106
#define LM_MAINDEVICE 0x08A4
#define LM_DEVICELIST 0x08A8
#define LM_THEGDEVICE 0x0CC8

/* CGrafPort offsets */
enum { CP_PIXMAP = 2, CP_VERSION = 6, CP_GRAFVARS = 8, CP_PNLOCHFRAC = 14, CP_PORTRECT = 16,
       CP_VISRGN = 24, CP_CLIPRGN = 28, CP_BKPIXPAT = 32, CP_RGBFG = 36, CP_RGBBK = 42,
       CP_PNLOC = 48, CP_PNSIZE = 52, CP_PNMODE = 56, CP_PNPIXPAT = 58, CP_FILLPIXPAT = 62,
       CP_PNVIS = 66, CP_TXFONT = 68, CP_TXFACE = 70, CP_TXMODE = 72, CP_TXSIZE = 74,
       CP_SPEXTRA = 76, CP_FGCOLOR = 80, CP_BKCOLOR = 84, CP_COLRBIT = 88 };
/* PixMap offsets */
enum { PM_BASE = 0, PM_ROWBYTES = 4, PM_BOUNDS = 6, PM_VERSION = 14, PM_PACKTYPE = 16,
       PM_PACKSIZE = 18, PM_HRES = 22, PM_VRES = 26, PM_PIXELTYPE = 30, PM_PIXELSIZE = 32,
       PM_CMPCOUNT = 34, PM_CMPSIZE = 36, PM_PLANEBYTES = 38, PM_TABLE = 42, PM_SIZE = 50 };

static uint32_t pop32(void){ uint32_t v = m68k_r32(SP); SP += 4; return v; }
static uint16_t pop16(void){ uint16_t v = (uint16_t)m68k_r16(SP); SP += 2; return v; }
static void push32(uint32_t v){ SP -= 4; m68k_w32(SP, v); }

static Rect rd_rect(uint32_t p){
    Rect r; r.top = (int16_t)m68k_r16(p); r.left = (int16_t)m68k_r16(p+2);
    r.bottom = (int16_t)m68k_r16(p+4); r.right = (int16_t)m68k_r16(p+6); return r; }
static void wr_rect(uint32_t p, int t, int l, int b, int r){
    m68k_w16(p, (uint16_t)t); m68k_w16(p+2, (uint16_t)l);
    m68k_w16(p+4, (uint16_t)b); m68k_w16(p+6, (uint16_t)r); }
static int rect_empty(const Rect *r){ return r->right <= r->left || r->bottom <= r->top; }

/* A handle from the HAL's Memory Manager (so GetHandleSize knows it). The
 * caller's registers are the game's: keep them. */
static uint32_t new_handle(uint32_t n){
    uint32_t d0 = M.d[0], a0 = M.a[0];
    M.d[0] = n; mr_toolbox_trap(0xA122);          /* NewHandle (zeroed by the bump heap) */
    uint32_t h = M.a[0];
    M.d[0] = d0; M.a[0] = a0;
    return h;
}
static uint32_t deref(uint32_t h){ return h ? m68k_r32(h) : 0; }

/* ---- the standard 8-bit colour table ('clut' 8) ----
 * 6x6x6 levels of $FF,$CC,$99,$66,$33,$00 for indices 0..214 (white first),
 * then ramps of red, green, blue and grey (10 steps each, skipping the cube's
 * levels), black at 255. The table every 8-bit Mac boots with. */
static void std_clut(int i, int *r, int *g, int *b){
    static const int lv[6] = { 0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00 };
    static const int ramp[10] = { 0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11 };
    if(i < 215){ *r = lv[i / 36]; *g = lv[(i / 6) % 6]; *b = lv[i % 6]; return; }
    if(i == 255){ *r = *g = *b = 0; return; }
    int k = (i - 215) % 10, which = (i - 215) / 10;
    *r = *g = *b = 0;
    if(which == 0) *r = ramp[k]; else if(which == 1) *g = ramp[k];
    else if(which == 2) *b = ramp[k]; else *r = *g = *b = ramp[k];
}

/* The device colour table: ctSeed(4) ctFlags(2) ctSize(2), then 256 ColorSpecs
 * of value(2) red(2) green(2) blue(2). */
static uint32_t ct_entry(int i){ return deref(G.ctab) + 8 + (uint32_t)i * 8; }
static void ct_set(int i, int r16, int g16, int b16){
    uint32_t e = ct_entry(i);
    m68k_w16(e, (uint16_t)i); m68k_w16(e+2, (uint16_t)r16); m68k_w16(e+4, (uint16_t)g16); m68k_w16(e+6, (uint16_t)b16);
}
static void ct_bump_seed(void){ uint32_t c = deref(G.ctab); m68k_w32(c, m68k_r32(c) + 1); }

/* For the presenter: the device's colours, as 0xAARRGGBB. */
void pop_colour_clut(uint32_t out[256]){
    for(int i = 0; i < 256; i++){
        uint32_t e = ct_entry(i);
        out[i] = 0xFF000000u | (uint32_t)(m68k_r16(e+2) >> 8) << 16 | (uint32_t)(m68k_r16(e+4) >> 8) << 8 | (m68k_r16(e+6) >> 8);
    }
}
int pop_colour_screen(uint32_t *base, int *w, int *h, int *rb){
    if(!g_mode) return 0;
    *base = G.scr; *w = SW; *h = SH; *rb = SRB; return 1;
}

/* Color2Index: the nearest device colour. Pure white and black are the fixed
 * ends of every Mac colour table (0 and 255), which no palette animation
 * touches; anything else is the nearest entry. */
static int color2index(int r16, int g16, int b16){
    int r = r16 >> 8, g = g16 >> 8, b = b16 >> 8;
    if(r == 255 && g == 255 && b == 255) return 0;
    if(!r && !g && !b) return 255;
    int best = 0; long bd = 1L << 30;
    for(int i = 0; i < 256; i++){
        uint32_t e = ct_entry(i);
        long dr = r - (m68k_r16(e+2) >> 8), dg = g - (m68k_r16(e+4) >> 8), db = b - (m68k_r16(e+6) >> 8);
        long d = dr*dr*3 + dg*dg*4 + db*db*2;
        if(d < bd){ bd = d; best = i; if(!d) break; }
    }
    return best;
}

/* ---- set-up: the screen and its GDevice (from pop_init) ---- */
void pop_cqd_init(int mode){
    g_mode = mode;
    if(!g_mode) return;
    SW = g_mode == 1 ? 640 : 512; SH = g_mode == 1 ? 480 : 384; SRB = SW;
    G.scr = mr_alloc((uint32_t)SRB * (uint32_t)SH);
    memset(M.mem + G.scr, 0xFF, (size_t)SRB * (size_t)SH);       /* black until the game paints */

    G.ctab = new_handle(8 + 256 * 8);
    uint32_t c = deref(G.ctab);
    m68k_w32(c, 0x1000);                                         /* ctSeed */
    m68k_w16(c + 4, 0x8000);                                     /* ctFlags: a device's table */
    m68k_w16(c + 6, 255);                                        /* ctSize: entries - 1 */
    for(int i = 0; i < 256; i++){ int r, g, b; std_clut(i, &r, &g, &b); ct_set(i, r * 0x101, g * 0x101, b * 0x101); }

    G.pmh = new_handle(PM_SIZE);
    uint32_t pm = deref(G.pmh);
    m68k_w32(pm + PM_BASE, G.scr);
    m68k_w16(pm + PM_ROWBYTES, (uint16_t)(0x8000 | SRB));
    wr_rect(pm + PM_BOUNDS, 0, 0, SH, SW);
    m68k_w32(pm + PM_HRES, 72u << 16); m68k_w32(pm + PM_VRES, 72u << 16);
    m68k_w16(pm + PM_PIXELTYPE, 0);                              /* chunky, indexed */
    m68k_w16(pm + PM_PIXELSIZE, 8); m68k_w16(pm + PM_CMPCOUNT, 1); m68k_w16(pm + PM_CMPSIZE, 8);
    m68k_w32(pm + PM_TABLE, G.ctab);

    /* GDevice: gdRefNum(2) gdID(2) gdType(2) gdITable(4) gdResPref(2)
     * gdSearchProc(4) gdCompProc(4) gdFlags(2) gdPMap(4) gdRefCon(4)
     * gdNextGD(4) gdRect(8) gdMode(4) ... 62 bytes. */
    G.gdh = new_handle(62);
    uint32_t gd = deref(G.gdh);
    m68k_w16(gd + 4, 0);                                         /* clutType */
    m68k_w16(gd + 20, 0xB801);                                   /* main, screen, active, colour */
    m68k_w32(gd + 22, G.pmh);
    m68k_w32(gd + 30, 0);                                        /* no next device */
    wr_rect(gd + 34, 0, 0, SH, SW);
    m68k_w32(gd + 42, 0x84);                                     /* 8 bits per pixel mode */

    m68k_w32(LM_SCRNBASE, G.scr);
    m68k_w16(LM_SCREENROW, (uint16_t)SRB);
    m68k_w32(LM_MAINDEVICE, G.gdh); m68k_w32(LM_DEVICELIST, G.gdh); m68k_w32(LM_THEGDEVICE, G.gdh);
    fprintf(stderr, "[cqd] colour screen %dx%d at %06x (%s)\n", SW, SH, G.scr,
            g_mode == 1 ? "Mac II 13\", Persia(COLOR)" : "Mac LC 12\", Persia(LC)");
}

/* ---- ports ---- */
static uint32_t cur_port(void){
    uint32_t slot = SP - 64;
    push32(slot); mr_toolbox_trap(0xA874);         /* GetPort */
    return m68k_r32(slot);
}
int pop_port_is_colour(uint32_t p){ return g_mode && p && (m68k_r16(p + CP_VERSION) & 0xC000) == 0xC000; }

/* A PixMap for a port: a copy of the device's, with bounds placing local
 * (0,0) at global (top, left). */
static uint32_t new_port_pixmap(int top, int left){
    uint32_t h = new_handle(PM_SIZE);
    memcpy(M.mem + deref(h), M.mem + deref(G.pmh), PM_SIZE);
    uint32_t pm = deref(h);
    wr_rect(pm + PM_BOUNDS, -top, -left, SH - top, SW - left);
    return h;
}
static uint32_t rect_rgn(int t, int l, int b, int r){
    Rect x = { (int16_t)t, (int16_t)l, (int16_t)b, (int16_t)r };
    return pop_qd_rect_rgn(&x);
}
/* The colour half of a port (OpenCPort, and a colour window). */
static void init_cport(uint32_t p, int top, int left, int h, int w, int keep_rgns){
    m68k_w16(p, 0);                                         /* device */
    m68k_w32(p + CP_PIXMAP, new_port_pixmap(top, left));
    m68k_w16(p + CP_VERSION, 0xC000);
    m68k_w32(p + CP_GRAFVARS, new_handle(36));
    m68k_w16(p + 12, 0); m68k_w16(p + CP_PNLOCHFRAC, 0x8000);
    wr_rect(p + CP_PORTRECT, 0, 0, h, w);
    if(!keep_rgns || !m68k_r32(p + CP_VISRGN)) m68k_w32(p + CP_VISRGN, rect_rgn(0, 0, h, w));
    else pop_qd_set_rect_rgn(m68k_r32(p + CP_VISRGN), 0, 0, h, w);
    /* A new port's clipRgn is wide open (Inside Macintosh I-163). */
    if(!keep_rgns || !m68k_r32(p + CP_CLIPRGN)) m68k_w32(p + CP_CLIPRGN, rect_rgn(-32767, -32767, 32767, 32767));
    else pop_qd_set_rect_rgn(m68k_r32(p + CP_CLIPRGN), -32767, -32767, 32767, 32767);
    m68k_w32(p + CP_BKPIXPAT, 0);
    for(int i = 0; i < 6; i++){ m68k_w8(p + CP_RGBFG + i, 0); m68k_w8(p + CP_RGBBK + i, 0xFF); }
    m68k_w32(p + CP_PNLOC, 0); m68k_w32(p + CP_PNSIZE, 0x00010001);
    m68k_w16(p + CP_PNMODE, 8);                             /* patCopy */
    m68k_w32(p + CP_PNPIXPAT, 0); m68k_w32(p + CP_FILLPIXPAT, 0); m68k_w16(p + CP_PNVIS, 0);
    m68k_w16(p + CP_TXFONT, 0); m68k_w16(p + CP_TXFACE, 0); m68k_w16(p + CP_TXMODE, 1); m68k_w16(p + CP_TXSIZE, 0);
    m68k_w32(p + CP_SPEXTRA, 0);
    m68k_w32(p + CP_FGCOLOR, 255); m68k_w32(p + CP_BKCOLOR, 0);
    m68k_w16(p + CP_COLRBIT, 0);
    for(uint32_t o = 90; o < 108; o++) m68k_w8(p + o, 0);   /* patStretch, picSave .. grafProcs */
}

/* The Window Manager port: the whole screen, global coordinates. */
static uint32_t wmgr_port(void){
    if(!G.wmgr){ G.wmgr = mr_alloc(108); init_cport(G.wmgr, 0, 0, SH, SW, 0); }
    return G.wmgr;
}

/* ---- pixels ---- */
typedef struct { uint8_t *base; int rb; Rect b; int depth; uint32_t gbase; } Pix;
/* A BitMap or PixMap pointer (what CopyBits is handed, and portBits). */
static int pix_of(uint32_t bm, Pix *x){
    uint32_t base = m68k_r32(bm); uint16_t rb = (uint16_t)m68k_r16(bm + 4);
    x->gbase = base;
    x->rb = rb & 0x3FFF; x->b = rd_rect(bm + 6);
    x->depth = (rb & 0x8000) ? (int)m68k_r16(bm + PM_PIXELSIZE) : 1;
    if(x->depth != 1 && x->depth != 8) return 0;
    uint64_t need = (uint64_t)x->rb * (uint64_t)(x->b.bottom - x->b.top);
    if(!base || base + need > M.memsize || x->rb <= 0) return 0;
    x->base = M.mem + base;
    return 1;
}
/* The pixels of a colour port. */
static int port_pix(uint32_t p, Pix *x){ uint32_t pm = deref(m68k_r32(p + CP_PIXMAP)); return pm && pix_of(pm, x); }

static inline int px_get(const Pix *x, int h, int v){
    if(h < x->b.left || h >= x->b.right || v < x->b.top || v >= x->b.bottom) return 0;
    int lx = h - x->b.left, ly = v - x->b.top;
    if(x->depth == 8) return x->base[ly * x->rb + lx];
    return (x->base[ly * x->rb + (lx >> 3)] >> (7 - (lx & 7))) & 1;
}
static inline void px_put(const Pix *x, int h, int v, int c){
    if(h < x->b.left || h >= x->b.right || v < x->b.top || v >= x->b.bottom) return;
    int lx = h - x->b.left, ly = v - x->b.top;
    if(x->depth == 8){ x->base[ly * x->rb + lx] = (uint8_t)c; return; }
    uint8_t m = (uint8_t)(0x80u >> (lx & 7)); uint8_t *a = &x->base[ly * x->rb + (lx >> 3)];
    *a = c ? (uint8_t)(*a | m) : (uint8_t)(*a & ~m);
}

/* The clip in force for the current port: QuickDraw's clip (clipRgn, and the
 * update region during BeginUpdate -- see qd.c), within the port's pixels. */
static Rect draw_clip(const Pix *x){
    Rect c; qd_get_clip(&c); rect_sect(&c, &x->b, &c);
    return c;
}

/* Fill a rectangle in the current colour port: 0 paint (fgColor), 1 erase
 * (bkColor), 2 invert. 1 if the port was a colour port. */
int pop_cqd_fill(const Rect *r, int how){
    uint32_t p = cur_port();
    if(!pop_port_is_colour(p)) return 0;
    Pix x; if(!port_pix(p, &x)) return 1;
    Rect c = draw_clip(&x), d;
    if(!rect_sect(r, &c, &d)) return 1;
    int col = how == 0 ? (int)m68k_r32(p + CP_FGCOLOR) : (int)m68k_r32(p + CP_BKCOLOR);
    for(int v = d.top; v < d.bottom; v++){
        uint8_t *row = x.base + (v - x.b.top) * x.rb + (d.left - x.b.left);
        if(how == 2){ for(int h = 0; h < d.right - d.left; h++) row[h] = (uint8_t)~row[h]; }
        else memset(row, col, (size_t)(d.right - d.left));
    }
    return 1;
}

/* ---- CopyBits for anything with a PixMap in it ----
 * Called by qd.c when the source or the destination is a PixMap. Transfer
 * modes as Colour QuickDraw applies them to indexed pixels: srcCopy copies,
 * srcOr copies all but white, srcBic turns the source's non-white into the
 * background, srcXor inverts, the not- forms take the source inverted; a
 * 1-bit source is coloured with the port's fore- and background colours. No
 * colour translation between tables: the game gives its offscreen PixMap the
 * device's own table while its window is active (CODE 2 0x1ab6). */
int pop_cqd_copybits(uint32_t srcB, uint32_t dstB, const Rect *sr, const Rect *dr, int mode,
                     const Rect *mask, int nmask){
    Pix s, d;
    if(!pix_of(srcB, &s) || !pix_of(dstB, &d)) return 1;
    if(s.depth == 1 && d.depth == 1) return 0;          /* qd.c's own business */
    int sw = sr->right - sr->left, sh = sr->bottom - sr->top;
    int dw = dr->right - dr->left, dh = dr->bottom - dr->top;
    if(sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 1;
    uint32_t p = cur_port();
    int fg = 255, bk = 0;
    if(pop_port_is_colour(p)){ fg = (int)m68k_r32(p + CP_FGCOLOR) & 255; bk = (int)m68k_r32(p + CP_BKCOLOR) & 255; }
    /* Clip: drawing into the current port's own pixels is clipped as QuickDraw
     * clips that port; anywhere else, only by the destination's bounds. */
    Rect clip = d.b;
    if(p){
        uint32_t pb = pop_port_is_colour(p) ? deref(deref(m68k_r32(p + CP_PIXMAP))) : m68k_r32(p + 2);
        if(pb == d.gbase){ Rect c; qd_get_clip(&c); rect_sect(&c, &clip, &clip); }
    }
    Rect dd; if(!rect_sect(dr, &clip, &dd)) return 1;
    int m = mode & 0x3F;
    /* The common case, a straight 8-bit copy of the same size: rows. */
    if(s.depth == 8 && d.depth == 8 && m == 0 && sw == dw && sh == dh && !mask){
        for(int v = dd.top; v < dd.bottom; v++){
            int sv = sr->top + (v - dr->top), sh0 = sr->left + (dd.left - dr->left);
            int n = dd.right - dd.left, h0 = dd.left;
            /* keep inside the source's bounds */
            if(sv < s.b.top || sv >= s.b.bottom) continue;
            if(sh0 < s.b.left){ int k = s.b.left - sh0; sh0 += k; h0 += k; n -= k; }
            if(sh0 + n > s.b.right) n = s.b.right - sh0;
            if(n <= 0) continue;
            memmove(d.base + (v - d.b.top) * d.rb + (h0 - d.b.left),
                    s.base + (sv - s.b.top) * s.rb + (sh0 - s.b.left), (size_t)n);
        }
        return 1;
    }
    for(int v = dd.top; v < dd.bottom; v++){
        int sv = sr->top + (v - dr->top) * sh / dh;
        for(int h = dd.left; h < dd.right; h++){
            if(mask){
                int in = 0;
                for(int k = 0; k < nmask && !in; k++) in = pt_in_rect(h, v, &mask[k]);
                if(!in) continue;
            }
            int sh_ = sr->left + (h - dr->left) * sw / dw;
            int sp = px_get(&s, sh_, sv);
            if(d.depth == 1){                               /* colour into a bitmap: dark is ink */
                int bit = s.depth == 1 ? sp : (sp != 0);
                if(m & 4) bit = !bit;
                switch(m & 3){
                case 0: px_put(&d, h, v, bit); break;
                case 1: if(bit) px_put(&d, h, v, 1); break;
                case 2: if(bit) px_put(&d, h, v, !px_get(&d, h, v)); break;
                case 3: if(bit) px_put(&d, h, v, 0); break;
                }
                continue;
            }
            if(s.depth == 1){                               /* a bitmap, coloured */
                int bit = sp;
                if(m == 36){ if(bit) px_put(&d, h, v, fg); continue; }     /* transparent */
                if(m & 4) bit = !bit;
                switch(m & 3){
                case 0: px_put(&d, h, v, bit ? fg : bk); break;
                case 1: if(bit) px_put(&d, h, v, fg); break;
                case 2: if(bit) px_put(&d, h, v, ~px_get(&d, h, v) & 255); break;
                case 3: if(bit) px_put(&d, h, v, bk); break;
                }
                continue;
            }
            /* 8-bit into 8-bit */
            if(m == 36){ if(sp != bk) px_put(&d, h, v, sp); continue; }    /* transparent */
            if(m & 4) sp = ~sp & 255;
            switch(m & 3){
            case 0: px_put(&d, h, v, sp); break;
            case 1: if(sp != 0) px_put(&d, h, v, sp); break;
            case 2: px_put(&d, h, v, px_get(&d, h, v) ^ sp); break;
            case 3: if(sp != 0) px_put(&d, h, v, bk); break;
            }
        }
    }
    return 1;
}

/* ---- text, in the game's own bitmap fonts ----
 * FONT resource (Inside Macintosh I-227): fontType firstChar lastChar widMax
 * kernMax nDescent fRectWidth fRectHeight owTLoc ascent descent leading
 * rowWords, the strike, the location table, the offset/width table. A face is
 * family*128 + size; when it is missing, the game's "Persian" 16. */
typedef struct { const uint8_t *b; int len, first, last, kern, height, ascent, rowbytes;
                 const uint8_t *bits, *loc, *ow; } Font;
#define BE16S(p) ((int)(int16_t)((p)[0] << 8 | (p)[1]))
static int font_open(int family, int size, Font *f){
    int len; const uint8_t *b = NULL;
    if(family || size) b = pop_res("FONT", family * 128 + size, &len);
    if(!b || len < 26) b = pop_res("FONT", 25616, &len);
    if(!b || len < 26) return 0;
    f->b = b; f->len = len;
    f->first = BE16S(b + 2); f->last = BE16S(b + 4); f->kern = BE16S(b + 8);
    f->height = BE16S(b + 14); f->ascent = BE16S(b + 18);
    f->rowbytes = BE16S(b + 24) * 2;
    f->bits = b + 26;
    f->loc = f->bits + f->rowbytes * f->height;
    f->ow = b + 16 + BE16S(b + 16) * 2;
    return f->loc + (f->last - f->first + 3) * 2 <= b + len && f->height > 0 && f->height < 128;
}
/* glyph c: strike columns [x0,x1), advance, offset. The missing-glyph glyph
 * (lastChar + 1) stands in for characters the font lacks. */
static int font_glyph(const Font *f, int c, int *x0, int *x1, int *adv, int *off){
    if(c < f->first || c > f->last) c = f->last + 1;
    int i = c - f->first;
    const uint8_t *ow = f->ow + i * 2;
    if(ow[0] == 0xFF && ow[1] == 0xFF){
        if(c == f->last + 1) return 0;
        return font_glyph(f, f->last + 1, x0, x1, adv, off);
    }
    *x0 = BE16S(f->loc + i * 2); *x1 = BE16S(f->loc + i * 2 + 2);
    *off = ow[0]; *adv = ow[1];
    return 1;
}
static int text_width(uint32_t p, const uint8_t *s, int n){
    Font f; if(!font_open((int16_t)m68k_r16(p + CP_TXFONT), (int16_t)m68k_r16(p + CP_TXSIZE), &f)) return n * 6;
    int w = 0;
    for(int i = 0; i < n; i++){ int a, b, adv, off; if(font_glyph(&f, s[i], &a, &b, &adv, &off)) w += adv; }
    return w;
}
static void draw_text(uint32_t p, const uint8_t *s, int n){
    int ph, pv; qd_get_pen(&ph, &pv);
    Font f; Pix x;
    if(!font_open((int16_t)m68k_r16(p + CP_TXFONT), (int16_t)m68k_r16(p + CP_TXSIZE), &f) || !port_pix(p, &x)){
        qd_pen_to(ph + n * 6, pv); return; }
    Rect c = draw_clip(&x);
    int mode = (int16_t)m68k_r16(p + CP_TXMODE) & 0x3F;
    int fg = (int)m68k_r32(p + CP_FGCOLOR) & 255, bk = (int)m68k_r32(p + CP_BKCOLOR) & 255;
    for(int i = 0; i < n; i++){
        int a, b, adv, off;
        if(!font_glyph(&f, s[i], &a, &b, &adv, &off)) continue;
        int gx = ph + f.kern + off, gy = pv - f.ascent;
        if(mode == 0 || mode == 4){                  /* srcCopy: the cell's background too */
            Rect cell = { (int16_t)gy, (int16_t)ph, (int16_t)(gy + f.height), (int16_t)(ph + adv) }, q;
            if(rect_sect(&cell, &c, &q))
                for(int v = q.top; v < q.bottom; v++) for(int h = q.left; h < q.right; h++)
                    px_put(&x, h, v, mode == 0 ? bk : fg);
        }
        for(int row = 0; row < f.height; row++)
            for(int col = a; col < b; col++){
                if(!((f.bits[row * f.rowbytes + (col >> 3)] >> (7 - (col & 7))) & 1)) continue;
                int h = gx + (col - a), v = gy + row;
                if(!pt_in_rect(h, v, &c)) continue;
                switch(mode){
                case 0: case 1: case 8: px_put(&x, h, v, fg); break;              /* srcCopy/srcOr/patCopy */
                case 2: px_put(&x, h, v, ~px_get(&x, h, v) & 255); break;         /* srcXor */
                case 3: px_put(&x, h, v, bk); break;                               /* srcBic */
                case 4: px_put(&x, h, v, bk); break;                               /* notSrcCopy */
                default: px_put(&x, h, v, fg); break;
                }
            }
        ph += adv;
    }
    qd_pen_to(ph, pv);
}

/* ---- the Palette Manager ----
 * Palette: pmEntries(2), pmDataFields(14), then per entry a ColorInfo of
 * ciRGB(6) ciUsage(2) ciTolerance(2) ciDataFields(6) = 16 bytes.
 * Entry i is shown through device index i, so ActivatePalette puts entry i's
 * colour into device entry i (for every usage but courteous) and
 * AnimatePalette changes the entries that are pmAnimated, on the device too.
 * The game reads the index back (PmForeColor, then the port's fgColor), so
 * it never relies on any particular assignment. */
enum { pmCourteous = 0, pmTolerant = 2, pmAnimated = 4, pmExplicit = 8 };
static uint32_t pal_entry(uint32_t pal, int i){ return deref(pal) + 16 + (uint32_t)i * 16; }
static int pal_count(uint32_t pal){ return pal && deref(pal) ? (int16_t)m68k_r16(deref(pal)) : 0; }

static uint32_t window_palette(uint32_t w){
    for(int i = 0; i < 8; i++) if(G.pal_win[i] == w) return G.pal_pal[i];
    return 0;
}
static void activate(uint32_t pal){
    int n = pal_count(pal); if(n > 256) n = 256;
    int changed = 0;
    for(int i = 0; i < n; i++){
        uint32_t e = pal_entry(pal, i);
        if(m68k_r16(e + 6) == pmCourteous) continue;
        uint32_t ce = ct_entry(i);
        for(int k = 0; k < 3; k++)
            if(m68k_r16(ce + 2 + k*2) != m68k_r16(e + k*2)){ m68k_w16(ce + 2 + k*2, (uint16_t)m68k_r16(e + k*2)); changed = 1; }
    }
    if(changed) ct_bump_seed();
}
static void animate(uint32_t pal, int entry, uint32_t rgb){
    if(!pal || entry < 0 || entry >= pal_count(pal) || entry > 255) return;
    uint32_t e = pal_entry(pal, entry);
    if(!(m68k_r16(e + 6) & pmAnimated)) return;
    for(int k = 0; k < 6; k += 2){ m68k_w16(e + (uint32_t)k, (uint16_t)m68k_r16(rgb + (uint32_t)k)); m68k_w16(ct_entry(entry) + 2 + (uint32_t)k, (uint16_t)m68k_r16(rgb + (uint32_t)k)); }
}

/* ---- the traps ---- */
static void trace_trap(const char *what){
    static int n = -1;
    if(n < 0){ const char *e = getenv("POPCQD"); n = e ? atoi(e) : 0; if(e && !n) n = 200; }
    if(n > 0){ n--; fprintf(stderr, "[cqd] %6.2f %s\n", pop_wall_ticks() / 60.0, what); }
}

int pop_cqd_trap(uint16_t w){
    if(!g_mode) return 0;
    switch(w){
    /* ---- Graphics Devices ---- */
    case 0xAA2A: case 0xAA29: case 0xAA32:   /* GetMainDevice / GetDeviceList / GetGDevice: GDHandle */
        m68k_w32(SP, w == 0xAA32 ? (m68k_r32(LM_THEGDEVICE) ? m68k_r32(LM_THEGDEVICE) : G.gdh) : G.gdh); return 1;
    case 0xAA2B: (void)pop32(); m68k_w32(SP, 0); return 1;            /* GetNextDevice: none */
    case 0xAA31: { uint32_t g = pop32(); m68k_w32(LM_THEGDEVICE, g ? g : G.gdh); return 1; }   /* SetGDevice */

    /* ---- ports ---- */
    case 0xAA00: { /* OpenCPort(port) */
        uint32_t p = pop32();
        if(!p) return 1;
        init_cport(p, 0, 0, SH, SW, 0);
        push32(p); mr_toolbox_trap(0xA873);                   /* and it becomes the current port */
        trace_trap("OpenCPort");
        return 1; }
    case 0xA910: { /* GetWMgrPort(VAR port): the screen, so painting the desktop reaches it */
        uint32_t pp = pop32(); if(pp) m68k_w32(pp, wmgr_port()); return 1; }
    case 0xAA48: { /* GetCWMgrPort */
        uint32_t pp = pop32(); if(pp) m68k_w32(pp, wmgr_port()); return 1; }

    /* ---- colours ---- */
    case 0xA862: case 0xA863: { /* ForeColor / BackColor(color: LONGINT), the eight old colours */
        uint32_t c = pop32(), p = cur_port();
        if(!p) return 1;
        static const struct { uint32_t k; int r, g, b; } OLD[] = {
            { 33, 0, 0, 0 }, { 30, 0xFFFF, 0xFFFF, 0xFFFF }, { 205, 0xDD6B, 0x08C2, 0x06A2 },
            { 341, 0x0000, 0x8000, 0x11B0 }, { 409, 0x0000, 0x0000, 0xD400 }, { 273, 0x0241, 0xAB54, 0xEAFF },
            { 137, 0xF2D7, 0x0856, 0x84EC }, { 69, 0xFC00, 0xF37D, 0x052F } };
        if(!pop_port_is_colour(p)){ m68k_w32(p + (w == 0xA862 ? CP_FGCOLOR : CP_BKCOLOR), c); return 1; }
        int r = 0, g = 0, b = 0;
        for(unsigned i = 0; i < sizeof OLD / sizeof *OLD; i++) if(OLD[i].k == c){ r = OLD[i].r; g = OLD[i].g; b = OLD[i].b; }
        if(c == 30){ r = g = b = 0xFFFF; }
        uint32_t rgb = p + (w == 0xA862 ? CP_RGBFG : CP_RGBBK);
        m68k_w16(rgb, (uint16_t)r); m68k_w16(rgb + 2, (uint16_t)g); m68k_w16(rgb + 4, (uint16_t)b);
        m68k_w32(p + (w == 0xA862 ? CP_FGCOLOR : CP_BKCOLOR), (uint32_t)color2index(r, g, b));
        return 1; }
    case 0xAA14: case 0xAA15: { /* RGBForeColor / RGBBackColor(VAR color) */
        uint32_t c = pop32(), p = cur_port();
        if(!pop_port_is_colour(p)) return 1;
        uint32_t rgb = p + (w == 0xAA14 ? CP_RGBFG : CP_RGBBK);
        for(int k = 0; k < 6; k++) m68k_w8(rgb + (uint32_t)k, m68k_r8(c + (uint32_t)k));
        m68k_w32(p + (w == 0xAA14 ? CP_FGCOLOR : CP_BKCOLOR),
                 (uint32_t)color2index(m68k_r16(c), m68k_r16(c + 2), m68k_r16(c + 4)));
        return 1; }
    case 0xAA33: { /* Color2Index(VAR rgb): LONGINT */
        uint32_t c = pop32(); m68k_w32(SP, (uint32_t)color2index(m68k_r16(c), m68k_r16(c + 2), m68k_r16(c + 4))); return 1; }
    case 0xAA34: { /* Index2Color(index: LONGINT; VAR rgb) */
        uint32_t c = pop32(); int i = (int)(pop32() & 255);
        for(int k = 0; k < 6; k++) m68k_w8(c + (uint32_t)k, m68k_r8(ct_entry(i) + 2 + (uint32_t)k)); return 1; }

    /* ---- drawing into a colour port ---- */
    case 0xA8A2: case 0xA8A3: case 0xA8A4: { /* PaintRect / EraseRect / InverRect(r) */
        if(!pop_port_is_colour(cur_port())) return 0;
        Rect r = rd_rect(pop32());
        pop_cqd_fill(&r, w == 0xA8A2 ? 0 : w == 0xA8A3 ? 1 : 2);
        return 1; }
    case 0xA8A5: { /* FillRect(r, pat): the pattern's ink is taken as the foreground */
        if(!pop_port_is_colour(cur_port())) return 0;
        (void)pop32(); Rect r = rd_rect(pop32()); pop_cqd_fill(&r, 0); return 1; }
    case 0xA884: { /* DrawString(s) */
        uint32_t p = cur_port(); if(!pop_port_is_colour(p)) return 0;
        uint32_t s = pop32(); uint8_t buf[256]; int n = m68k_r8(s);
        for(int i = 0; i < n; i++) buf[i] = (uint8_t)m68k_r8(s + 1 + (uint32_t)i);
        draw_text(p, buf, n); return 1; }
    case 0xA883: { /* DrawChar(ch) */
        uint32_t p = cur_port(); if(!pop_port_is_colour(p)) return 0;
        uint8_t c = (uint8_t)pop16(); draw_text(p, &c, 1); return 1; }
    case 0xA885: { /* DrawText(buf, first, count) */
        uint32_t p = cur_port(); if(!pop_port_is_colour(p)) return 0;
        int cnt = (int16_t)pop16(), first = (int16_t)pop16(); uint32_t b = pop32();
        uint8_t buf[256]; if(cnt > 255) cnt = 255; if(cnt < 0) cnt = 0;
        for(int i = 0; i < cnt; i++) buf[i] = (uint8_t)m68k_r8(b + (uint32_t)(first + i));
        draw_text(p, buf, cnt); return 1; }
    case 0xA88C: { /* StringWidth(s): INTEGER */
        uint32_t p = cur_port(); if(!pop_port_is_colour(p)) return 0;
        uint32_t s = pop32(); uint8_t buf[256]; int n = m68k_r8(s);
        for(int i = 0; i < n; i++) buf[i] = (uint8_t)m68k_r8(s + 1 + (uint32_t)i);
        m68k_w16(SP, (uint16_t)text_width(p, buf, n)); return 1; }
    case 0xA88D: { /* CharWidth(ch): INTEGER */
        uint32_t p = cur_port(); if(!pop_port_is_colour(p)) return 0;
        uint8_t c = (uint8_t)pop16(); m68k_w16(SP, (uint16_t)text_width(p, &c, 1)); return 1; }

    /* ---- the Palette Manager ---- */
    case 0xAA91: { /* NewPalette(entries, srcColors: CTabHandle, srcUsage, srcTolerance): PaletteHandle */
        int tol = (int16_t)pop16(), usage = (int16_t)pop16(); uint32_t ct = pop32(); int n = (int16_t)pop16();
        if(n < 0) n = 0;
        uint32_t h = new_handle(16 + (uint32_t)n * 16), pal = deref(h);
        m68k_w16(pal, (uint16_t)n);
        uint32_t c = deref(ct); int csz = c ? (int16_t)m68k_r16(c + 6) + 1 : 0;
        for(int i = 0; i < n; i++){
            uint32_t e = pal + 16 + (uint32_t)i * 16;
            if(i < csz) for(int k = 0; k < 6; k++) m68k_w8(e + (uint32_t)k, m68k_r8(c + 8 + (uint32_t)i * 8 + 2 + (uint32_t)k));
            m68k_w16(e + 6, (uint16_t)usage); m68k_w16(e + 8, (uint16_t)tol);
        }
        m68k_w32(SP, h);
        trace_trap("NewPalette");
        return 1; }
    case 0xAA9E: { /* SetEntryUsage(pal, entry, usage, tolerance) */
        int tol = (int16_t)pop16(), usage = (int16_t)pop16(), i = (int16_t)pop16(); uint32_t pal = pop32();
        if(pal && i >= 0 && i < pal_count(pal)){
            uint32_t e = pal_entry(pal, i);
            if(usage != -1) m68k_w16(e + 6, (uint16_t)usage);
            if(tol != -1) m68k_w16(e + 8, (uint16_t)tol);
        }
        return 1; }
    case 0xAA95: { /* SetPalette(window, pal, cUpdates) */
        (void)pop16(); uint32_t pal = pop32(), win = pop32();
        int k = -1;
        for(int i = 0; i < 8; i++) if(G.pal_win[i] == win){ k = i; break; }
        for(int i = 0; i < 8 && k < 0; i++) if(!G.pal_win[i]){ k = i; }
        if(k >= 0){ G.pal_win[k] = win; G.pal_pal[k] = pal; }
        activate(pal);
        trace_trap("SetPalette");
        return 1; }
    case 0xAA96: { /* GetPalette(window): PaletteHandle */
        uint32_t win = pop32(); m68k_w32(SP, window_palette(win)); return 1; }
    case 0xAA94: { /* ActivatePalette(window) */
        uint32_t win = pop32(); activate(window_palette(win)); trace_trap("ActivatePalette"); return 1; }
    case 0xAA93: (void)pop32(); return 1;             /* DisposePalette: the bump heap keeps it */
    case 0xAA24: (void)pop32(); return 1;             /* DisposeCTable */
    case 0xAA9A: { /* AnimatePalette(window, srcCTab, srcIndex, dstEntry, dstLength) */
        int len = (int16_t)pop16(), dst = (int16_t)pop16(), src = (int16_t)pop16();
        uint32_t ct = pop32(), win = pop32(), pal = window_palette(win), c = deref(ct);
        { char t[80]; snprintf(t, sizeof t, "AnimatePalette %d..%d from %d", dst, dst + len - 1, src); trace_trap(t); }
        if(c){
            int csz = (int16_t)m68k_r16(c + 6) + 1;
            for(int i = 0; i < len && src + i < csz; i++)
                animate(pal, dst + i, c + 8 + (uint32_t)(src + i) * 8 + 2);
        }
        return 1; }
    case 0xAA99: { /* AnimateEntry(window, entry, VAR rgb) */
        uint32_t rgb = pop32(); int i = (int16_t)pop16(); uint32_t win = pop32();
        { char t[80]; snprintf(t, sizeof t, "AnimateEntry %d = %04x %04x %04x", i, m68k_r16(rgb), m68k_r16(rgb+2), m68k_r16(rgb+4)); trace_trap(t); }
        animate(window_palette(win), i, rgb); return 1; }
    case 0xAA97: { /* PmForeColor(entry) */
        int i = (int16_t)pop16(); uint32_t p = cur_port(), pal = 0;
        if(!pop_port_is_colour(p)) return 1;
        /* The palette of the window in front, as the real thing does for a
         * port that is not a window. */
        for(int k = 0; k < 8 && !pal; k++) if(G.pal_win[k] == p) pal = G.pal_pal[k];
        for(int k = 0; k < 8 && !pal; k++) if(G.pal_win[k]) pal = G.pal_pal[k];
        int idx = i & 255;
        m68k_w32(p + CP_FGCOLOR, (uint32_t)idx);
        uint32_t src = (pal && i < pal_count(pal)) ? pal_entry(pal, i) : ct_entry(idx) + 2;
        for(int k = 0; k < 6; k++) m68k_w8(p + CP_RGBFG + (uint32_t)k, m68k_r8(src + (uint32_t)k));
        return 1; }
    case 0xAA98: { /* PmBackColor(entry) */
        int i = (int16_t)pop16(); uint32_t p = cur_port();
        if(pop_port_is_colour(p)) m68k_w32(p + CP_BKCOLOR, (uint32_t)(i & 255));
        return 1; }
    case 0xAA37: { /* GetSubTable(myColors, iTabRes, targetTbl): each value := nearest index */
        uint32_t tgt = pop32(); (void)pop16(); uint32_t mine = pop32(), c = deref(mine);
        if(c){
            int n = (int16_t)m68k_r16(c + 6) + 1;
            uint32_t t = deref(tgt); int tn = t ? (int16_t)m68k_r16(t + 6) + 1 : 0;
            for(int i = 0; i < n; i++){
                uint32_t e = c + 8 + (uint32_t)i * 8;
                int r = m68k_r16(e + 2) >> 8, g = m68k_r16(e + 4) >> 8, b = m68k_r16(e + 6) >> 8;
                if(!t){ m68k_w16(e, (uint16_t)color2index(m68k_r16(e + 2), m68k_r16(e + 4), m68k_r16(e + 6))); continue; }
                int best = 0; long bd = 1L << 30;
                for(int j = 0; j < tn; j++){
                    uint32_t q = t + 8 + (uint32_t)j * 8;
                    long dr = r - (m68k_r16(q + 2) >> 8), dg = g - (m68k_r16(q + 4) >> 8), db = b - (m68k_r16(q + 6) >> 8);
                    long d = dr*dr + dg*dg + db*db;
                    if(d < bd){ bd = d; best = j; }
                }
                m68k_w16(e, (uint16_t)best);
            }
        }
        return 1; }
    }
    return 0;
}

/* NewCWindow: the HAL's NewWindow does the Window Manager's bookkeeping (and
 * leaves the arguments' result slot filled); then the port becomes a colour
 * port onto the screen. Called with the arguments still on the stack. */
uint32_t pop_cqd_new_cwindow(void){
    Rect b = rd_rect(m68k_r32(SP + 18));          /* boundsRect, under title(4) visible(2) procID(2) behind(4) goAway(2) refCon(4) */
    mr_toolbox_trap(0xA913);
    uint32_t win = m68k_r32(SP);
    if(!win) return 0;
    init_cport(win, b.top, b.left, b.bottom - b.top, b.right - b.left, 1);
    trace_trap("NewCWindow");
    return win;
}

/* SetOrigin on a colour port: the PixMap's bounds move with portRect. */
int pop_cqd_set_origin(uint32_t p, int dh, int dv){
    if(!pop_port_is_colour(p)) return 0;
    uint32_t pm = deref(m68k_r32(p + CP_PIXMAP));
    if(pm){
        Rect r = rd_rect(pm + PM_BOUNDS);
        wr_rect(pm + PM_BOUNDS, r.top + dv, r.left + dh, r.bottom + dv, r.right + dh);
    }
    return 1;
}

/* InitGraf: in colour, screenBits describes the colour screen (its baseAddr,
 * the row width without the PixMap flag, the device's rectangle). */
void pop_cqd_initgraf(uint32_t gp){
    if(!g_mode || gp < 128) return;
    uint32_t sb = gp - 122;
    m68k_w32(sb, G.scr); m68k_w16(sb + 4, (uint16_t)SRB); wr_rect(sb + 6, 0, 0, SH, SW);
}

/* SysEnvirons, after the HAL has filled the record in: a colour Mac. */
void pop_cqd_sysenvirons(uint32_t rec){
    if(!g_mode || !rec) return;
    m68k_w16(rec + 2, g_mode == 1 ? 6 : 19);      /* envMacII / envMacLC */
    m68k_w16(rec + 6, 3);                         /* env68020: the colour code is 68020 code */
    m68k_w8(rec + 9, 1);                          /* hasColorQD */
}

/* The screen as a picture, for headless checks (POPSHOTS): 1 if written. */
int pop_cqd_shot(const char *path){
    if(!g_mode) return 0;
    FILE *f = fopen(path, "wb"); if(!f) return 1;
    uint32_t pal[256]; pop_colour_clut(pal);
    fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    for(int y = 0; y < SH; y++) for(int x = 0; x < SW; x++){
        uint32_t c = pal[M.mem[G.scr + (uint32_t)y * (uint32_t)SRB + (uint32_t)x]];
        fputc((int)(c >> 16) & 255, f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f);
    }
    fclose(f);
    return 1;
}
