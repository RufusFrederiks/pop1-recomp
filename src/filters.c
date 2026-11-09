/* filters.c - pixel-art scalers for the Remastered looks (on the CPU).
 *
 * Each filter turns a w x h ARGB frame into a kw x kh one (k = 3 or 4,
 * whichever is closest to the size on screen), but only for the
 * source rows [y0, y1): platform.c calls it for the band of rows that
 * changed since the last frame (widened by the filter's reach), so a frame
 * where only the prince moved costs a few rows, not the whole screen.
 *
 *  - AdvMAME: 3x is Scale3x, 4x is Scale2x applied twice (AdvanceMAME,
 *    Andrea Mazzoleni).
 *    Only exact colour matches count, so it never invents colours: the
 *    pixel look stays, diagonal stair-steps become clean lines.
 *  - xBR: the xBR algorithm by Hyllian (MIT licence), level 2 edge
 *    rules, written for this project. It finds edges by comparing colour
 *    differences along the two diagonals and blends the corner of each
 *    pixel towards the colour across the edge, giving smooth curves.
 *  - MMPX: "MMPX Style-Preserving Pixel Art Magnification" by Morgan
 *    McGuire and Mara Gagiu (Journal of Graphics Techniques, 2021; the
 *    reference code is MIT licensed), 2x applied twice for 4x. Like
 *    AdvMAME it only ever copies a pixel of the source, so every colour it
 *    outputs is one the frame already had; its rules are made for drawn
 *    pixel art: 1:1 and 2:1 slopes become clean lines, while lone pixels,
 *    thin lines, crossings and the tips of shapes are kept as drawn. */
#include "filters.h"
#include <stdlib.h>
#include <string.h>

static inline int clampi(int v, int lo, int hi){ return v < lo ? lo : v > hi ? hi : v; }

/* ---- Scale2x / AdvMAME ---- */

/* rows [y0, y1) of src (w x h) -> rows [2*y0, 2*y1) of dst (2w x 2h) */
static void scale2x(const uint32_t *src, int w, int h, uint32_t *dst, int y0, int y1){
    int dw = w * 2;
    for(int y = y0; y < y1; y++){
        const uint32_t *up = src + clampi(y - 1, 0, h - 1) * w, *row = src + y * w,
                       *dn = src + clampi(y + 1, 0, h - 1) * w;
        uint32_t *o0 = dst + (2 * y) * dw, *o1 = o0 + dw;
        for(int x = 0; x < w; x++){
            int xl = x > 0 ? x - 1 : 0, xr = x < w - 1 ? x + 1 : w - 1;
            uint32_t B = up[x], D = row[xl], E = row[x], F = row[xr], H = dn[x];
            if(B != H && D != F){
                o0[2 * x]     = D == B ? D : E;
                o0[2 * x + 1] = B == F ? F : E;
                o1[2 * x]     = D == H ? D : E;
                o1[2 * x + 1] = H == F ? F : E;
            } else {
                o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = E;
            }
        }
    }
}

/* Scale3x: rows [y0, y1) of src -> rows [3*y0, 3*y1) of dst (3w x 3h) */
static void scale3x(const uint32_t *src, int w, int h, uint32_t *dst, int y0, int y1){
    int dw = w * 3;
    for(int y = y0; y < y1; y++){
        const uint32_t *up = src + clampi(y - 1, 0, h - 1) * w, *row = src + y * w,
                       *dn = src + clampi(y + 1, 0, h - 1) * w;
        uint32_t *o0 = dst + (3 * y) * dw, *o1 = o0 + dw, *o2 = o1 + dw;
        for(int x = 0; x < w; x++){
            int xl = x > 0 ? x - 1 : 0, xr = x < w - 1 ? x + 1 : w - 1;
            uint32_t A = up[xl], B = up[x], C = up[xr], D = row[xl], E = row[x], F = row[xr],
                     G = dn[xl], H = dn[x], I = dn[xr];
            uint32_t *a = o0 + 3 * x, *b = o1 + 3 * x, *c = o2 + 3 * x;
            if(B != H && D != F){
                a[0] = D == B ? D : E;
                a[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                a[2] = B == F ? F : E;
                b[0] = (D == B && E != G) || (D == H && E != A) ? D : E;
                b[1] = E;
                b[2] = (B == F && E != I) || (H == F && E != C) ? F : E;
                c[0] = D == H ? D : E;
                c[1] = (D == H && E != I) || (H == F && E != G) ? H : E;
                c[2] = H == F ? F : E;
            } else {
                a[0] = a[1] = a[2] = b[0] = b[1] = b[2] = c[0] = c[1] = c[2] = E;
            }
        }
    }
}

void filter_advmame(const uint32_t *src, int w, int h, uint32_t *dst, int k, int y0, int y1){
    if(k == 3){ scale3x(src, w, h, dst, y0, y1); return; }
    static uint32_t *mid; static size_t midn;
    size_t need = (size_t)w * 2 * h * 2;
    if(midn < need){ free(mid); mid = malloc(need * 4); midn = mid ? need : 0; if(!mid) return; }
    /* the second pass reads one 2x row above and below its band */
    scale2x(src, w, h, mid, clampi(y0 - 1, 0, h), clampi(y1 + 1, 0, h));
    scale2x(mid, w * 2, h * 2, dst, 2 * y0, 2 * y1);
}

/* ---- xBR ---- */

/* Colour difference, weighted towards brightness the way xBR does (YUV with
 * Y counted ~7x more than U and V). Linear, so it works on the RGB deltas. */
static inline int df(uint32_t a, uint32_t b){
    int r = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
    int g = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
    int bl = (int)(a & 255) - (int)(b & 255);
    int y = (299 * r + 587 * g + 114 * bl) / 1000;
    int u = (-169 * r - 331 * g + 500 * bl) / 1000;
    int v = (500 * r - 419 * g - 81 * bl) / 1000;
    return 48 * abs(y) + 7 * abs(u) + 6 * abs(v);
}
#define XBR_EQ 1536                      /* "about the same colour": a brightness step of ~32/255 */
static inline int eq(uint32_t a, uint32_t b){ return df(a, b) < XBR_EQ; }

/* a + (b - a) * n / 8, per channel */
static inline uint32_t mix8(uint32_t a, uint32_t b, int n){
    uint32_t o = 0xFF000000;
    for(int s = 0; s < 24; s += 8){
        int ca = (a >> s) & 255, cb = (b >> s) & 255;
        o |= (uint32_t)(ca + (cb - ca) * n / 8) << s;
    }
    return o;
}
#define mix4(a, b, n) mix8(a, b, 2 * (n))

/* Neighbours, written for the bottom-right corner of pixel E:
 *            B
 *        D   E   F   F4
 *        G   H   I   I4
 *                H5  I5          (C is above F)
 * The other three corners use the same rules with the picture turned by
 * 90, 180 and 270 degrees. */
enum { NE, NI, NH, NF, NG, NC, ND, NB, NF4, NH5, NI4, NI5, NN };
static const int BASE[NN][2] = {
    { 0, 0 }, { 1, 1 }, { 0, 1 }, { 1, 0 }, { -1, 1 }, { 1, -1 }, { -1, 0 }, { 0, -1 },
    { 2, 0 }, { 0, 2 }, { 2, 1 }, { 1, 2 },
};
/* Cells of the output block, for the bottom-right corner (row, col):
 * 4x4 (B15 is the corner itself) and 3x3 (T8 is the corner). */
enum { B15, B14, B13, B12, B11, B10, B7, B3, NB_ };
static const int BBASE[NB_][2] = { { 3, 3 }, { 3, 2 }, { 3, 1 }, { 3, 0 }, { 2, 3 }, { 2, 2 }, { 1, 3 }, { 0, 3 } };
enum { T8, T7, T6, T5, T2, NT_ };
static const int TBASE[NT_][2] = { { 2, 2 }, { 2, 1 }, { 2, 0 }, { 1, 2 }, { 0, 2 } };
static int g_nb[4][NN][2], g_blk[4][NB_], g_blk3[4][NT_];
static void xbr_tables(void){
    static int done; if(done) return; done = 1;
    for(int r = 0; r < 4; r++){
        for(int k = 0; k < NN; k++){
            int x = BASE[k][0], y = BASE[k][1];
            for(int t = 0; t < r; t++){ int nx = -y, ny = x; x = nx; y = ny; }   /* turn 90 degrees */
            g_nb[r][k][0] = x; g_nb[r][k][1] = y;
        }
        for(int k = 0; k < NB_; k++){
            int x = 2 * BBASE[k][1] - 3, y = 2 * BBASE[k][0] - 3;   /* around the block's centre */
            for(int t = 0; t < r; t++){ int nx = -y, ny = x; x = nx; y = ny; }
            g_blk[r][k] = ((y + 3) / 2) * 4 + (x + 3) / 2;
        }
        for(int k = 0; k < NT_; k++){
            int x = TBASE[k][1] - 1, y = TBASE[k][0] - 1;
            for(int t = 0; t < r; t++){ int nx = -y, ny = x; x = nx; y = ny; }
            g_blk3[r][k] = (y + 1) * 3 + (x + 1);
        }
    }
}

static void xbr_corner(uint32_t *blk, int k, const int *bi, const uint32_t *P){
    uint32_t E = P[NE], F = P[NF], H = P[NH];
    if(E == F || E == H) return;
    uint32_t I = P[NI], G = P[NG], C = P[NC], D = P[ND], B = P[NB];
    int e = df(E, C) + df(E, G) + df(I, P[NH5]) + df(I, P[NF4]) + 4 * df(H, F);   /* change along / */
    int i = df(H, D) + df(H, P[NI5]) + df(F, P[NI4]) + df(F, B) + 4 * df(E, I);   /* change along \ */
    if(e > i) return;                                    /* no edge across this corner */
    uint32_t px = df(E, F) <= df(E, H) ? F : H;
    if(e < i && ((!eq(F, B) && !eq(H, D)) || (eq(E, I) && !eq(F, P[NI4]) && !eq(H, P[NI5])) || eq(E, G) || eq(E, C))){
        int ke = df(F, G), ki = df(H, C);
        int left = 2 * ke <= ki && E != G && D != G;     /* a shallow edge */
        int up   = ke >= 2 * ki && E != C && B != C;     /* a steep edge */
        if(k == 3){
            uint32_t *t8 = &blk[bi[T8]], *t7 = &blk[bi[T7]], *t6 = &blk[bi[T6]], *t5 = &blk[bi[T5]], *t2 = &blk[bi[T2]];
            if(left && up){
                *t7 = mix8(*t7, px, 6); *t6 = mix8(*t6, px, 2);
                *t5 = *t7; *t2 = *t6; *t8 = px;
            } else if(left){
                *t7 = mix8(*t7, px, 6); *t5 = mix8(*t5, px, 2); *t6 = mix8(*t6, px, 2); *t8 = px;
            } else if(up){
                *t5 = mix8(*t5, px, 6); *t7 = mix8(*t7, px, 2); *t2 = mix8(*t2, px, 2); *t8 = px;
            } else {
                *t8 = mix8(*t8, px, 7); *t5 = mix8(*t5, px, 1); *t7 = mix8(*t7, px, 1);
            }
            return;
        }
        uint32_t *b15 = &blk[bi[B15]], *b14 = &blk[bi[B14]], *b13 = &blk[bi[B13]], *b12 = &blk[bi[B12]],
                 *b11 = &blk[bi[B11]], *b10 = &blk[bi[B10]], *b7 = &blk[bi[B7]], *b3 = &blk[bi[B3]];
        if(left && up){
            *b13 = mix4(*b13, px, 3); *b12 = mix4(*b12, px, 1);
            *b15 = *b14 = *b11 = px;
            *b10 = *b3 = *b12; *b7 = *b13;
        } else if(left){
            *b11 = mix4(*b11, px, 3); *b13 = mix4(*b13, px, 3);
            *b10 = mix4(*b10, px, 1); *b12 = mix4(*b12, px, 1);
            *b14 = *b15 = px;
        } else if(up){
            *b14 = mix4(*b14, px, 3); *b7 = mix4(*b7, px, 3);
            *b10 = mix4(*b10, px, 1); *b3 = mix4(*b3, px, 1);
            *b11 = *b15 = px;
        } else {                                         /* a 45-degree edge */
            *b11 = mix4(*b11, px, 2); *b14 = mix4(*b14, px, 2);
            *b15 = px;
        }
    } else {
        int c = bi[k == 3 ? T8 : B15];
        blk[c] = mix8(blk[c], px, 4);
    }
}

void filter_xbr(const uint32_t *src, int w, int h, uint32_t *dst, int k, int y0, int y1){
    xbr_tables();
    int dw = w * k;
    for(int y = y0; y < y1; y++)
        for(int x = 0; x < w; x++){
            uint32_t blk[16], E = src[y * w + x];
            for(int i = 0; i < k * k; i++) blk[i] = E;
            for(int r = 0; r < 4; r++){
                uint32_t P[NN];
                for(int n = 0; n < NN; n++)
                    P[n] = src[clampi(y + g_nb[r][n][1], 0, h - 1) * w + clampi(x + g_nb[r][n][0], 0, w - 1)];
                xbr_corner(blk, k, k == 3 ? g_blk3[r] : g_blk[r], P);
            }
            uint32_t *o = dst + (k * y) * dw + k * x;
            for(int r = 0; r < k; r++) memcpy(o + r * dw, blk + r * k, (size_t)k * 4);
        }
}

/* ---- MMPX ---- */

/* Brightness, for the rules that let the darker colour win (outlines stay
 * whole). The frame is opaque, so the reference's alpha term drops out. */
static inline int luma(uint32_t c){ return (int)(((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255)) + 1; }
static inline int all_eq2(uint32_t b, uint32_t a0, uint32_t a1){ return b == a0 && b == a1; }
static inline int all_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2){ return b == a0 && b == a1 && b == a2; }
static inline int all_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3){
    return b == a0 && b == a1 && b == a2 && b == a3; }
static inline int any_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2){ return b == a0 || b == a1 || b == a2; }
static inline int none_eq2(uint32_t b, uint32_t a0, uint32_t a1){ return b != a0 && b != a1; }
static inline int none_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3){
    return b != a0 && b != a1 && b != a2 && b != a3; }

/* rows [y0, y1) of src (w x h) -> rows [2*y0, 2*y1) of dst (2w x 2h).
 * Reads up to 3 pixels away; past the edges the edge pixel repeats.
 * Not reentrant (one padded ring of lines); platform.c calls it from one
 * thread.
 * Neighbours of E (the reference's names):
 *                P
 *            A   B   C
 *        Q   D   E   F   R
 *            G   H   I
 *                S
 * Output: J K (top left, top right), L M (bottom left, bottom right). */
static void mmpx2x(const uint32_t *src, int w, int h, uint32_t *dst, int y0, int y1){
    int dw = w * 2;
    /* The seven rows around y, each padded with 3 copies of its edge pixels
     * on both sides, so the loop needs no bounds checks. A ring of 8 lines:
     * moving down a row pads one new line. */
    static uint32_t *pad; static int padw;
    if(padw < w){ free(pad); pad = malloc((size_t)(w + 6) * 8 * 4); padw = pad ? w : 0; if(!pad) return; }
    const uint32_t *r[7];
    int filled = 0;                          /* rows y0-3 .. are in the ring up to here */
    for(int y = y0; y < y1; y++){
        for(int yy = y0 - 3 + filled; yy <= y + 3; yy++, filled++){
            uint32_t *l = pad + (size_t)((yy + 8) & 7) * (w + 6);
            const uint32_t *s = src + clampi(yy, 0, h - 1) * w;
            memcpy(l + 3, s, (size_t)w * 4);
            l[0] = l[1] = l[2] = s[0];
            l[w + 3] = l[w + 4] = l[w + 5] = s[w - 1];
        }
        for(int i = 0; i < 7; i++) r[i] = pad + (size_t)((y - 3 + i + 8) & 7) * (w + 6) + 3;
        uint32_t *o0 = dst + (2 * y) * dw, *o1 = o0 + dw;
        #define PX(dx, dy) r[3 + (dy)][x + (dx)]
        for(int x = 0; x < w; x++){
            uint32_t A = PX(-1, -1), B = PX(0, -1), C = PX(1, -1),
                     D = PX(-1, 0),  E = PX(0, 0),  F = PX(1, 0),
                     G = PX(-1, 1),  H = PX(0, 1),  I = PX(1, 1);
            uint32_t J = E, K = E, L = E, M = E;
            if(((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0){
                uint32_t P = PX(0, -2), S = PX(0, 2), Q = PX(-2, 0), R = PX(2, 0);
                int Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

                /* 1:1 slopes */
                if((D == B && D != H && D != F) && (El >= Dl || E == A) && any_eq3(E, A, C, G) &&
                   (El < Dl || A != D || E != P || E != Q)) J = D;
                if((B == F && B != D && B != H) && (El >= Bl || E == C) && any_eq3(E, A, C, I) &&
                   (El < Bl || C != B || E != P || E != R)) K = B;
                if((H == D && H != F && H != B) && (El >= Hl || E == G) && any_eq3(E, A, G, I) &&
                   (El < Hl || G != H || E != S || E != Q)) L = H;
                if((F == H && F != B && F != D) && (El >= Fl || E == I) && any_eq3(E, C, G, I) &&
                   (El < Fl || I != H || E != R || E != S)) M = F;

                /* crossings: a line through E's row or column stays whole */
                if((E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && F != PX(3, 0)) K = M = F;
                if((E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && D != PX(-3, 0)) J = L = D;
                if((E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && H != PX(0, 3)) L = M = H;
                if((E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && B != PX(0, -3)) J = K = B;

                /* the tips of triangles */
                if(Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) J = K = B;
                if(Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) L = M = H;
                if(Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) K = M = F;
                if(Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) J = L = D;

                /* 2:1 slopes */
                if(H != B){
                    if(H != A && H != E && H != C){
                        if(all_eq3(H, G, F, R) && none_eq2(H, D, PX(2, -1))) L = M;
                        if(all_eq3(H, I, D, Q) && none_eq2(H, F, PX(-2, -1))) M = L;
                    }
                    if(B != I && B != G && B != E){
                        if(all_eq3(B, A, F, R) && none_eq2(B, D, PX(2, 1))) J = K;
                        if(all_eq3(B, C, D, Q) && none_eq2(B, F, PX(-2, 1))) K = J;
                    }
                }
                if(F != D){
                    if(D != I && D != E && D != C){
                        if(all_eq3(D, A, H, S) && none_eq2(D, B, PX(1, 2))) J = L;
                        if(all_eq3(D, G, B, P) && none_eq2(D, H, PX(1, -2))) L = J;
                    }
                    if(F != E && F != A && F != G){
                        if(all_eq3(F, C, H, S) && none_eq2(F, B, PX(-1, 2))) K = M;
                        if(all_eq3(F, I, B, P) && none_eq2(F, H, PX(-1, -2))) M = K;
                    }
                }
            }
            o0[2 * x] = J; o0[2 * x + 1] = K;
            o1[2 * x] = L; o1[2 * x + 1] = M;
        }
        #undef PX
    }
}

/* 4x: MMPX twice. The 2x picture is kept between frames, so only the band
 * of it the changed rows reach is made again. A 4x output row depends on
 * source rows up to MMPX_REACH away (3 in each pass: 3 + 3/2, rounded up). */
void filter_mmpx(const uint32_t *src, int w, int h, uint32_t *dst, int y0, int y1){
    static uint32_t *mid; static size_t midn;
    size_t need = (size_t)w * 2 * h * 2;
    if(midn < need){ free(mid); mid = malloc(need * 4); midn = mid ? need : 0; if(!mid) return; }
    /* the second pass reads three 2x rows above and below its band */
    mmpx2x(src, w, h, mid, clampi(y0 - 2, 0, h), clampi(y1 + 2, 0, h));
    mmpx2x(mid, w * 2, h * 2, dst, 2 * y0, 2 * y1);
}
