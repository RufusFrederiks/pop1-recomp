/* filters.c - pixel-art scalers for the Remastered looks (4x, on the CPU).
 *
 * Each filter turns a w x h ARGB frame into a 4w x 4h one, but only for the
 * source rows [y0, y1): platform.c calls it for the band of rows that
 * changed since the last frame (widened by the filter's reach), so a frame
 * where only the prince moved costs a few rows, not the whole screen.
 *
 *  - AdvMAME 4x: Scale2x (AdvanceMAME, Andrea Mazzoleni) applied twice.
 *    Only exact colour matches count, so it never invents colours: the
 *    pixel look stays, diagonal stair-steps become clean lines.
 *  - xBR 4x: the xBR algorithm by Hyllian (MIT licence), level 2 edge
 *    rules, written for this project. It finds edges by comparing colour
 *    differences along the two diagonals and blends the corner of each
 *    pixel towards the colour across the edge, giving smooth curves. */
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

void filter_advmame4x(const uint32_t *src, int w, int h, uint32_t *dst, int y0, int y1){
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

/* a + (b - a) * n / 4, per channel */
static inline uint32_t mix4(uint32_t a, uint32_t b, int n){
    uint32_t o = 0xFF000000;
    for(int s = 0; s < 24; s += 8){
        int ca = (a >> s) & 255, cb = (b >> s) & 255;
        o |= (uint32_t)(ca + (cb - ca) * n / 4) << s;
    }
    return o;
}

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
/* Cells of the 4x4 output block, for the bottom-right corner (row, col). */
enum { B15, B14, B13, B12, B11, B10, B7, B3, NB_ };
static const int BBASE[NB_][2] = { { 3, 3 }, { 3, 2 }, { 3, 1 }, { 3, 0 }, { 2, 3 }, { 2, 2 }, { 1, 3 }, { 0, 3 } };
static int g_nb[4][NN][2], g_blk[4][NB_];
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
    }
}

static void xbr_corner(uint32_t *blk, const int *bi, const uint32_t *P){
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
        blk[bi[B15]] = mix4(blk[bi[B15]], px, 2);
    }
}

void filter_xbr4x(const uint32_t *src, int w, int h, uint32_t *dst, int y0, int y1){
    xbr_tables();
    int dw = w * 4;
    for(int y = y0; y < y1; y++)
        for(int x = 0; x < w; x++){
            uint32_t blk[16], E = src[y * w + x];
            for(int k = 0; k < 16; k++) blk[k] = E;
            for(int r = 0; r < 4; r++){
                uint32_t P[NN];
                for(int k = 0; k < NN; k++)
                    P[k] = src[clampi(y + g_nb[r][k][1], 0, h - 1) * w + clampi(x + g_nb[r][k][0], 0, w - 1)];
                xbr_corner(blk, g_blk[r], P);
            }
            uint32_t *o = dst + (4 * y) * dw + 4 * x;
            for(int r = 0; r < 4; r++) memcpy(o + r * dw, blk + r * 4, 16);
        }
}
