/* light.c - torch lighting: a recomp-only effect, never in the Original look.
 *
 * Where the torches are: the game keeps the whole level at A5-0x16E2 (the
 * LEVL resource, 2304 bytes, copied there at the start of each level, CODE 3
 * 0x3a8), in the layout every PoP shares: 24 rooms x 30 tiles of foreground,
 * then 24 x 30 modifiers, ... The room on screen is the word at A5-0x4596
 * (CODE 4 0x3256 loads its neighbours from it; the left one goes to
 * A5-0x4594). A tile's low 5 bits are its type: 19 a torch, 30 a torch with
 * debris, 10 a potion (its kind in the modifier's top 3 bits).
 *
 * Where they are on screen (measured on real frames): Colour 640x480 has
 * tiles 64 wide and rows 126 high from y = 40; a torch's flame is drawn at
 * x = 64 * col + 94, in the next tile's space, 28 below the row's top. LC
 * 512x384 is about 0.8 of that (rows of 104 from y = 25). A torch in the
 * left room's last column shows at the left edge.
 *
 * The room variable keeps its value outside play (the princess scenes, the
 * title), so nothing is lit on its word alone: a torch is lit once its
 * flame colour has been seen at its spot since the room came up, and the
 * whole effect is only on while the room's wall tiles and the status line
 * are on the screen too. It fades in and out over a quarter second.
 *
 * Drawing (SDL render blend modes only, no shaders):
 *  1. a light map at the game's own size: the room's dim light everywhere,
 *     with a soft warm pool added around each torch (and a faint one round a
 *     potion), white over the status line;
 *  2. the light map stretched over the picture (smoothly) and multiplied in
 *     (BLENDMODE_MOD): the room dims, the pools stay as drawn;
 *  3. warm light added on top (BLENDMODE_ADD): a faint cast on the stones
 *     around each torch, a small glow on the flame, a tiny one on a potion
 *     in its own colour.
 * The pools and glows flicker gently, each torch on its own.
 *
 * All the strengths are in TUNE below. */
#include "light.h"
#include "macrecomp/m68k.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.283185307179586

/* ---- what to tune ---- */
static const struct {
    int amb[3];            /* the room's light away from the torches, per channel (255 = as drawn) */
    double pool_rx, pool_ry, pool_dy;  /* pool size (in tiles / rows) and how far below the flame its centre is (rows) */
    int pool[3]; int pool_a;           /* the pool's colour and strength, added into the light map */
    double cast_rx, cast_ry, cast_dy; int cast[3]; int cast_a;   /* warm light added on the picture (size, centre below the flame, colour, strength) */
    double glow_r; int glow[3]; int glow_a;              /* the glow on the flame (radius in tiles) */
    double flicker;        /* how much the light breathes, +- (0.08 = 8%) */
    double potion_r; int potion_a;    /* a potion's glow (radius in tiles) and strength */
} TUNE = {
    { 214, 212, 222 },
    1.60, 0.78, 0.32, { 255, 226, 178 }, 110,
    1.00, 0.55, 0.15, { 255, 120, 30 }, 26,
    0.30, { 255, 150, 50 }, 60,
    0.08,
    0.45, 80,
};

/* ---- the screen layouts ---- */
typedef struct {
    int w, h;
    double tile_w, top, row_h;     /* tiles: x = tile_w * col, rows from top, row_h high */
    double flame_dx, flame_dy;     /* flame centre, from the tile's left and the row's top */
    double flame_hw, flame_hh;     /* half the box its colours are looked for in */
    double potion_dx, potion_dy;   /* potion (its liquid), from the tile's left and the row's top */
} Geo;
static const Geo GEOS[] = {
    { 640, 480, 64.0, 40, 126, 94, 28, 16, 22, 46.5, 100 },   /* Colour (Mac II) */
    { 512, 384, 51.2, 25, 104, 75, 23, 13, 18, 37.0,  80 },   /* LC */
};

enum { MAXSRC = 40 };
typedef struct { int kind, key; double x, y; int seen; uint32_t ok_ms; uint32_t colour; double phase; } Src;
static Src g_src[MAXSRC];
static int g_nsrc, g_room = -1, g_level = -1;
static const Geo *g_geo;
static double g_presence, g_fade = 1;
static uint32_t g_last_ms;
static int g_cached;                       /* the colour indices below are known */
static int g_orange = -1, g_yellow = -1;   /* the flame's two colours */
static uint8_t g_black[256], g_flame[256], g_flask[256];
static SDL_Texture *g_grad, *g_lmap;
static int g_lw, g_lh;

static int r16s(uint32_t a){ return (int)(int16_t)m68k_r16(a); }

/* The screen indices of the colours that matter, read from the colour table
 * the first time the flame's orange is in it (not in a fade). */
static void cache_colours(const uint32_t clut[256]){
    if(g_cached) return;
    int o = -1, y = -1;
    for(int i = 0; i < 256; i++){
        if((clut[i] | 0xFF000000) == 0xFFFF8500 && o < 0) o = i;
        if((clut[i] | 0xFF000000) == 0xFFFFF300 && y < 0) y = i;
    }
    if(o < 0 || y < 0) return;
    g_orange = o; g_yellow = y;
    for(int i = 0; i < 256; i++){
        uint32_t c = clut[i] | 0xFF000000;
        g_black[i] = c == 0xFF000000;
        g_flame[i] = i == o || i == y;
        g_flask[i] = c == 0xFFE68A3A || c == 0xFFFFC26E || c == 0xFFFFEBAB || c == 0xFF783D30;
    }
    g_black[2] = 1;                        /* the background (the hurt flash animates it) */
    g_cached = 1;
}

/* Count pixels of a set in a box of the screen. */
static int count_in(const uint8_t *scr, int w, int h, int rb, const uint8_t *set,
                    double cx, double cy, double hw, double hh){
    int x0 = (int)(cx - hw), x1 = (int)(cx + hw), y0 = (int)(cy - hh), y1 = (int)(cy + hh), n = 0;
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > w) x1 = w;
    if(y1 > h) y1 = h;
    for(int y = y0; y < y1; y++){ const uint8_t *row = scr + (size_t)y * rb;
        for(int x = x0; x < x1; x++) n += set[row[x]]; }
    return n;
}

/* The room's torches and potions, from the level in memory. Sources already
 * known (same room, same tile) keep what has been seen of them. */
static void read_room(void){
    uint32_t a5 = m68k_r32(0x904);              /* CurrentA5 */
    int room = r16s(a5 - 0x4596), level = r16s(a5 - 0x56F4);
    int same = room == g_room && level == g_level;
    Src old[MAXSRC]; int nold = same ? g_nsrc : 0;
    memcpy(old, g_src, sizeof old);
    g_nsrc = 0; g_room = room; g_level = level;
    if(room < 1 || room > 24) return;
    uint32_t fg = a5 - 0x16E2;
    const Geo *G = g_geo;
    int left = r16s(a5 - 0x4594);
    for(int i = -3; i < 30 && g_nsrc < MAXSRC; i++){
        int row, col, t, key;
        if(i < 0){                                 /* the left room's last column */
            if(left < 1 || left > 24) continue;
            row = i + 3; col = -1; key = 100 + row;
            t = m68k_r8(fg + (uint32_t)(left - 1) * 30 + (uint32_t)row * 10 + 9) & 31;
            if(t == 10) continue;                  /* only its torch's flame reaches in */
        } else {
            row = i / 10; col = i % 10; key = i;
            t = m68k_r8(fg + (uint32_t)(room - 1) * 30 + (uint32_t)i) & 31;
        }
        int kind = (t == 19 || t == 30) ? 0 : t == 10 ? 1 : -1;
        if(kind < 0) continue;
        Src *s = &g_src[g_nsrc];
        memset(s, 0, sizeof *s);
        s->kind = kind; s->key = key;
        if(kind == 0){ s->x = G->tile_w * col + G->flame_dx; s->y = G->top + G->row_h * row + G->flame_dy;
                       if(s->x - G->flame_hw >= G->w) continue; }   /* the last column's flame is off the room */
        else { s->x = G->tile_w * col + G->potion_dx; s->y = G->top + G->row_h * row + G->potion_dy; }
        s->phase = (key * 2.399 + room * 0.7);   /* every torch breathes on its own */
        for(int j = 0; j < nold; j++) if(old[j].kind == kind && old[j].key == key){
            s->seen = old[j].seen; s->ok_ms = old[j].ok_ms; s->colour = old[j].colour; }
        g_nsrc++;
    }
}

/* Is the level's room what the screen shows? Its wall tiles have to be on
 * the screen (not black), and the status line's health on the left. */
static int room_on_screen(const uint8_t *scr, int w, int h, int rb){
    uint32_t a5 = m68k_r32(0x904);
    int room = g_room;
    if(room < 1 || room > 24) return 0;
    const Geo *G = g_geo;
    static uint8_t notblack[256];
    for(int i = 0; i < 256; i++) notblack[i] = !g_black[i];
    double sy = G->top + 3 * G->row_h;
    if(count_in(scr, w, h, rb, notblack, G->tile_w * 0.6, sy + 8, G->tile_w * 0.6, 8) < 12) return 0;
    int walls = 0, good = 0;
    for(int i = 0; i < 30; i++){
        int t = m68k_r8(a5 - 0x16E2 + (uint32_t)(room - 1) * 30 + (uint32_t)i) & 31;
        if(t != 20) continue;
        walls++;
        double x0 = G->tile_w * (i % 10), y0 = G->top + G->row_h * (i / 10);
        int hit = 0;
        for(int a = 0; a < 4; a++) for(int b = 0; b < 3; b++){
            int x = (int)(x0 + G->tile_w * (0.2 + 0.2 * a)), y = (int)(y0 + G->row_h * (0.35 + 0.2 * b));
            if(x >= 0 && x < w && y >= 0 && y < h) hit += !g_black[scr[(size_t)y * rb + x]];
        }
        good += hit >= 8;
    }
    return walls == 0 || good * 10 >= walls * 7;
}

void light_update(int on, const uint8_t *scr, int w, int h, int rb, const uint32_t clut[256], int top){
    uint32_t now = SDL_GetTicks();
    double dt = g_last_ms ? (now - g_last_ms) / 1000.0 : 0;
    if(dt > 0.25) dt = 0.25;
    g_last_ms = now;
    g_geo = NULL;
    for(unsigned i = 0; i < sizeof GEOS / sizeof *GEOS; i++) if(GEOS[i].w == w && GEOS[i].h == h) g_geo = &GEOS[i];
    if(on && g_geo) cache_colours(clut);
    if(!on || !g_geo || !g_cached){ g_presence = 0; g_nsrc = 0; g_room = -1; return; }
    if(top || g_room < 0) read_room();
    const Geo *G = g_geo;
    int any = 0;
    for(int i = 0; i < g_nsrc; i++){
        Src *s = &g_src[i];
        if(s->kind == 0){
            /* seen once since the room came up: stays lit (the prince or a
             * guard may stand in front of the flame) */
            if(!s->seen && count_in(scr, w, h, rb, g_flame, s->x, s->y, G->flame_hw, G->flame_hh) >= 6) s->seen = 1;
            any |= s->seen;
        } else if(count_in(scr, w, h, rb, g_flask, s->x, s->y + G->row_h * 0.06, G->tile_w * 0.25, G->row_h * 0.16) >= 8){
            s->ok_ms = now;
            /* its glow takes the colour of the liquid: the most saturated
             * pixel round the bubbles that isn't the flask or the flame */
            int best = 40; uint32_t col = 0;
            int x0 = (int)(s->x - G->tile_w * 0.2), x1 = (int)(s->x + G->tile_w * 0.2);
            int y0 = (int)(s->y - G->row_h * 0.12), y1 = (int)(s->y + G->row_h * 0.08);
            for(int y = y0 < 0 ? 0 : y0; y < y1 && y < h; y++)
                for(int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++){
                    int c = scr[(size_t)y * rb + x];
                    if(g_flask[c] || g_flame[c] || g_black[c]) continue;
                    uint32_t v = clut[c]; int r = v >> 16 & 255, g = v >> 8 & 255, b = v & 255;
                    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
                    if(mx - mn > best){ best = mx - mn; col = v; }
                }
            if(col) s->colour = col;
        }
    }
    int potions = 0;
    for(int i = 0; i < g_nsrc; i++) potions |= g_src[i].kind == 1 && now - g_src[i].ok_ms < 400;
    int want = (any || potions || g_nsrc == 0) && room_on_screen(scr, w, h, rb);
    /* fades: the flame's orange in the colour table now, against its own */
    uint32_t o = clut[g_orange];
    g_fade = ((o >> 16 & 255) + (o >> 8 & 255)) / (255.0 + 0x85);
    if(g_fade > 1) g_fade = 1;
    double step = dt / 0.25;
    if(want) g_presence = g_presence + step > 1 ? 1 : g_presence + step;
    else     g_presence = g_presence - step < 0 ? 0 : g_presence - step;
}

int light_visible(void){ return g_presence > 0.004 && g_geo; }

/* A soft round spot: white, fading to nothing at its edge ((1 - r^2)^2). */
static SDL_Texture *gradient(SDL_Renderer *ren){
    enum { N = 128 };
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
    if(!t) return NULL;
    uint32_t *px = malloc(N * N * 4);
    if(!px){ SDL_DestroyTexture(t); return NULL; }
    for(int y = 0; y < N; y++) for(int x = 0; x < N; x++){
        double dx = (x + 0.5) / (N / 2) - 1, dy = (y + 0.5) / (N / 2) - 1, r2 = dx * dx + dy * dy;
        double v = r2 < 1 ? (1 - r2) * (1 - r2) : 0;
        int c = (int)(v * 255 + 0.5);
        px[y * N + x] = 0xFF000000u | (uint32_t)c << 16 | (uint32_t)c << 8 | (uint32_t)c;
    }
    SDL_UpdateTexture(t, NULL, px, N * 4);
    free(px);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
    return t;
}

/* A torch's breathing: two slow waves and a little smoothed noise. */
static double noise1(double t, double seed){
    int i = (int)floor(t); double f = t - i, u = f * f * (3 - 2 * f);
    double a = sin((i + seed) * 12.9898) * 43758.5453, b = sin((i + 1 + seed) * 12.9898) * 43758.5453;
    a -= floor(a); b -= floor(b);
    return (a + (b - a) * u) * 2 - 1;
}
static double flicker(const Src *s, double t){
    double v = 0.45 * sin(t * TAU * 0.9 + s->phase) + 0.25 * sin(t * TAU * 2.3 + 1.7 * s->phase)
             + 0.30 * noise1(t * 7.0, s->phase * 3.1);
    return 1 + TUNE.flicker * v;
}

static void spot(SDL_Renderer *ren, double cx, double cy, double rx, double ry, int k,
                 int r, int g, int b, double a){
    if(a <= 0) return;
    SDL_Rect d = { (int)((cx - rx) * k), (int)((cy - ry) * k), (int)(2 * rx * k + 0.5), (int)(2 * ry * k + 0.5) };
    SDL_SetTextureColorMod(g_grad, (Uint8)r, (Uint8)g, (Uint8)b);
    SDL_SetTextureAlphaMod(g_grad, (Uint8)(a > 255 ? 255 : a));
    SDL_RenderCopy(ren, g_grad, NULL, &d);
}

void light_draw(SDL_Renderer *ren, int k){
    if(!light_visible() || k < 1) return;
    const Geo *G = g_geo;
    if(!g_grad && !(g_grad = gradient(ren))) return;
    if(g_lmap && (g_lw != G->w || g_lh != G->h)){ SDL_DestroyTexture(g_lmap); g_lmap = NULL; }
    if(!g_lmap){
        g_lmap = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, G->w, G->h);
        if(!g_lmap) return;
        SDL_SetTextureScaleMode(g_lmap, SDL_ScaleModeLinear);
        g_lw = G->w; g_lh = G->h;
    }
    double p = g_presence, t = SDL_GetTicks() / 1000.0;
    SDL_Texture *dst = SDL_GetRenderTarget(ren);

    /* 1: the light map */
    SDL_SetRenderTarget(ren, g_lmap);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, (Uint8)(255 - (255 - TUNE.amb[0]) * p), (Uint8)(255 - (255 - TUNE.amb[1]) * p),
                           (Uint8)(255 - (255 - TUNE.amb[2]) * p), 255);
    SDL_RenderClear(ren);
    for(int i = 0; i < g_nsrc; i++){
        const Src *s = &g_src[i];
        if(s->kind == 0 && s->seen){
            double f = flicker(s, t);
            spot(ren, s->x, s->y + G->row_h * TUNE.pool_dy, G->tile_w * TUNE.pool_rx * (0.97 + 0.03 * f),
                 G->row_h * TUNE.pool_ry * (0.97 + 0.03 * f), 1, TUNE.pool[0], TUNE.pool[1], TUNE.pool[2], TUNE.pool_a * f * p);
        } else if(s->kind == 1 && SDL_GetTicks() - s->ok_ms < 400)
            spot(ren, s->x, s->y, G->tile_w * TUNE.potion_r * 1.6, G->row_h * TUNE.potion_r, 1, 255, 255, 255, 60 * p);
    }
    {   /* the status line keeps its own light */
        SDL_Rect st = { 0, (int)(G->top + 3 * G->row_h), G->w, G->h };
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderFillRect(ren, &st);
    }
    SDL_SetRenderTarget(ren, dst);

    /* 2: multiply it in */
    SDL_Rect all = { 0, 0, G->w * k, G->h * k };
    SDL_SetTextureBlendMode(g_lmap, SDL_BLENDMODE_MOD);
    SDL_RenderCopy(ren, g_lmap, NULL, &all);

    /* 3: warm light on top (less of it in a fade, none once faded out) */
    double q = p * g_fade * g_fade;
    for(int i = 0; i < g_nsrc; i++){
        const Src *s = &g_src[i];
        if(s->kind == 0 && s->seen){
            double f = flicker(s, t);
            spot(ren, s->x, s->y + G->row_h * TUNE.cast_dy, G->tile_w * TUNE.cast_rx, G->row_h * TUNE.cast_ry, k,
                 TUNE.cast[0], TUNE.cast[1], TUNE.cast[2], TUNE.cast_a * f * q);
            spot(ren, s->x, s->y, G->tile_w * TUNE.glow_r * (0.95 + 0.05 * f), G->tile_w * TUNE.glow_r * 1.25 * (0.95 + 0.05 * f), k,
                 TUNE.glow[0], TUNE.glow[1], TUNE.glow[2], TUNE.glow_a * f * f * q);
        } else if(s->kind == 1 && SDL_GetTicks() - s->ok_ms < 400 && s->colour){
            double f = 1 + 0.5 * TUNE.flicker * sin(t * TAU * 1.4 + s->phase);
            spot(ren, s->x, s->y, G->tile_w * TUNE.potion_r, G->tile_w * TUNE.potion_r, k,
                 s->colour >> 16 & 255, s->colour >> 8 & 255, s->colour & 255, TUNE.potion_a * f * q);
        }
    }
    SDL_SetTextureColorMod(g_grad, 255, 255, 255);
    SDL_SetTextureAlphaMod(g_grad, 255);
}

int light_sources(LightSrc *out, int max){
    int n = 0; uint32_t now = SDL_GetTicks();
    if(!light_visible()) return 0;
    for(int i = 0; i < g_nsrc && n < max; i++){
        const Src *s = &g_src[i];
        if(s->kind == 0 ? !s->seen : (now - s->ok_ms >= 400 || !s->colour)) continue;
        LightSrc *o = &out[n++];
        o->kind = s->kind; o->key = s->key; o->x = (float)s->x; o->y = (float)s->y; o->phase = (float)s->phase;
        o->r = (s->colour >> 16 & 255) / 255.f; o->g = (s->colour >> 8 & 255) / 255.f; o->b = (s->colour & 255) / 255.f;
    }
    return n;
}
float light_level(void){ return light_visible() ? (float)(g_presence * g_fade * g_fade) : 0; }
int light_room(unsigned char tiles[40], float *tile_w, float *top, float *row_h){
    if(!g_geo || g_room < 1 || g_room > 24) return 0;
    uint32_t a5 = m68k_r32(0x904), fg = a5 - 0x16E2;
    int up = r16s(a5 - 0x4590);
    for(int c = 0; c < 10; c++) tiles[c] = (up >= 1 && up <= 24) ? (m68k_r8(fg + (uint32_t)(up - 1) * 30 + 20 + (uint32_t)c) & 31) : 255;
    for(int i = 0; i < 30; i++) tiles[10 + i] = m68k_r8(fg + (uint32_t)(g_room - 1) * 30 + (uint32_t)i) & 31;
    *tile_w = (float)g_geo->tile_w; *top = (float)g_geo->top; *row_h = (float)g_geo->row_h;
    return g_level * 100 + g_room;
}

/* The game's two characters. Each is the 16-byte record every PoP has, here
 * 18 bytes apart: Kid at A5-0x46BA, Guard at A5-0x46A8 (found by watching
 * memory while the Prince walks; frame numbers are the DOS game's). Bytes:
 * 0 frame, 1 x, 2 y, 3 direction, 4 column, 5 row, 6 action, 9 room,
 * 13 alive (0xFF = alive). x is in the game's own units (14 to a tile, 58 at
 * the left edge), y the floor line of 200 (55, 118, 181). */
void light_chars(LightChar out[2]){
    static const uint32_t at[2] = { 0x46BA, 0x46A8 };
    out[0].there = out[1].there = 0;
    if(!g_geo || !light_visible() || g_room < 1) return;
    uint32_t a5 = m68k_r32(0x904);
    const Geo *G = g_geo;
    for(int i = 0; i < 2; i++){
        uint32_t c = a5 - at[i];
        int frame = m68k_r8(c), x = m68k_r8(c + 1), y = m68k_r8(c + 2), action = m68k_r8(c + 6),
            room = m68k_r8(c + 9), alive = (int8_t)m68k_r8(c + 13);
        if(!frame || room != g_room) continue;
        LightChar *o = &out[i];
        o->there = 1; o->frame = frame;
        o->x = (float)((x - 58) * G->tile_w / 14.0 + G->tile_w * 0.21);
        o->y = (float)(G->top + y * G->row_h / 63.0);
        o->grounded = action == 0 || action == 1 || action == 5 || action == 7;
        double h = 82;                                    /* standing, of 126 a row */
        if(frame >= 107 && frame <= 119) h = 50;          /* crouching */
        if(alive >= 0 || frame == 185) h = 0;             /* dead on the floor */
        o->h = (float)(h * G->row_h / 126.0);
    }
}

void light_free(void){
    if(g_grad) SDL_DestroyTexture(g_grad);
    if(g_lmap) SDL_DestroyTexture(g_lmap);
    g_grad = g_lmap = NULL; g_lw = g_lh = 0;
}
