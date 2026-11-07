/* crt.c - the CRT looks, drawn with the GPU (SDL render blend modes only).
 *
 * The picture is built on the screen in four layers:
 *   1. the game, scaled to the picture rectangle: from the sharp 4x stage
 *      (Monitor), or straight from the 1x frame with bilinear filtering,
 *      which gives the softer picture of a TV;
 *   2. the same again, added on top (BLENDMODE_ADD) to brighten it, because
 *      the next layer darkens;
 *   3. a mask the size of the picture, multiplied in (BLENDMODE_MOD): the
 *      scanlines (each game row a bright beam, darker between rows) times
 *      the phosphor pattern, made for that exact size so its stripes sit
 *      on whole screen pixels;
 *   4. glow: the frame shrunk to a quarter (two halvings, each averaging
 *      2x2 pixels) and stretched back over the picture, added on top.
 *
 * Monitor: modelled on the AppleColor RGB monitors of the time (Sony
 * Trinitron tubes): an aperture grille (fine vertical red, green and blue
 * stripes), light scanlines, a little glow. TV: a slot mask, strong
 * scanlines, a softer picture and more glow. */
#include "crt.h"

typedef struct {
    double sigma;        /* beam width, in game rows; smaller = darker gaps between rows */
    double mask_dim;     /* how far the two other colours are dimmed in each stripe */
    double slot_dim;     /* TV: the dark gaps of the slot mask (1 = none) */
    double boost;        /* layer 2: how much of the picture is added again */
    int glow;            /* layer 4: alpha of the glow, 0-255 */
} Kind;
static const Kind KINDS[2] = {
    { 0.42, 0.78, 1.00, 0.30,  45 },     /* Monitor */
    { 0.32, 0.74, 0.85, 0.40,  70 },     /* TV */
};

static SDL_Texture *g_half, *g_quarter, *g_mask;
static int g_hw, g_hh, g_mw, g_mh, g_mkind = -1, g_mvh;

void crt_free(void){
    if(g_half) SDL_DestroyTexture(g_half);
    if(g_quarter) SDL_DestroyTexture(g_quarter);
    if(g_mask) SDL_DestroyTexture(g_mask);
    g_half = g_quarter = g_mask = NULL;
    g_hw = g_hh = g_mw = g_mh = g_mvh = 0; g_mkind = -1;
}

static SDL_Texture *target(SDL_Renderer *ren, int w, int h, SDL_ScaleMode sm){
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if(t){ SDL_SetTextureScaleMode(t, sm); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE); }
    return t;
}

/* The mask for a picture of w x h screen pixels showing vh game rows. */
static int make_mask(SDL_Renderer *ren, int kind, int w, int h, int vh){
    if(g_mask && g_mw == w && g_mh == h && g_mkind == kind && g_mvh == vh) return 1;
    if(g_mask){ SDL_DestroyTexture(g_mask); g_mask = NULL; }
    g_mask = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if(!g_mask) return 0;
    SDL_SetTextureScaleMode(g_mask, SDL_ScaleModeNearest);
    const Kind *K = &KINDS[kind];
    uint32_t *px = malloc((size_t)w * h * 4);
    if(!px){ SDL_DestroyTexture(g_mask); g_mask = NULL; return 0; }
    int lines = h >= 2 * vh;                     /* below 2x there is no room for scanlines */
    for(int y = 0; y < h; y++){
        double scan = 1.0;
        if(lines){
            double sy = (y + 0.5) * vh / h, f = sy - (int)sy - 0.5;     /* distance from the row's centre */
            scan = exp(-f * f / (2 * K->sigma * K->sigma));
        }
        for(int x = 0; x < w; x++){
            int stripe = x % 3;
            double c[3] = { K->mask_dim, K->mask_dim, K->mask_dim };   /* r, g, b */
            c[stripe] = 1.0;
            double slot = 1.0;
            if(K->slot_dim < 1.0 && ((y + ((x / 3) & 1) * 2) & 3) == 3) slot = K->slot_dim;
            uint32_t v = 0xFF000000;
            for(int i = 0; i < 3; i++){
                int b = (int)(255.0 * scan * slot * c[i] + 0.5);
                v |= (uint32_t)(b > 255 ? 255 : b) << (16 - 8 * i);
            }
            px[y * w + x] = v;
        }
    }
    SDL_UpdateTexture(g_mask, NULL, px, w * 4);
    free(px);
    g_mw = w; g_mh = h; g_mkind = kind; g_mvh = vh;
    return 1;
}

int crt_draw(SDL_Renderer *ren, int kind, SDL_Texture *game, const SDL_Rect *view,
             SDL_Texture *stage, int k, const SDL_Rect *d){
    if(kind < 0 || kind > 1) return 0;
    const Kind *K = &KINDS[kind];
    int hw = view->w / 2, hh = view->h / 2;
    if(!g_half || g_hw != hw || g_hh != hh){
        if(g_half) SDL_DestroyTexture(g_half);
        if(g_quarter) SDL_DestroyTexture(g_quarter);
        g_half = target(ren, hw, hh, SDL_ScaleModeLinear);
        g_quarter = target(ren, hw / 2, hh / 2, SDL_ScaleModeLinear);
        g_hw = hw; g_hh = hh;
        if(!g_half || !g_quarter) return 0;
    }
    if(!make_mask(ren, kind, d->w, d->h, view->h)) return 0;

    /* glow: the frame at half, then quarter size, each step averaging 2x2 */
    SDL_SetTextureScaleMode(game, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(game, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(ren, g_half);
    SDL_RenderCopy(ren, game, view, NULL);
    SDL_SetRenderTarget(ren, g_quarter);
    SDL_SetTextureBlendMode(g_half, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(ren, g_half, NULL, NULL);
    SDL_SetRenderTarget(ren, NULL);

    /* 1 + 2: the picture, and some of it again */
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_Texture *src = game; SDL_Rect sr = *view;
    if(kind == 0 && stage){
        src = stage; sr.x = view->x * k; sr.y = view->y * k; sr.w = view->w * k; sr.h = view->h * k;
        SDL_SetTextureScaleMode(stage, SDL_ScaleModeLinear);
    }
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(ren, src, &sr, d);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_ADD);
    for(double left = K->boost; left > 0.01; left -= 1.0){
        SDL_SetTextureAlphaMod(src, (Uint8)(255 * (left > 1 ? 1 : left)));
        SDL_RenderCopy(ren, src, &sr, d);
    }
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);

    /* 3: scanlines and phosphors */
    SDL_SetTextureBlendMode(g_mask, SDL_BLENDMODE_MOD);
    SDL_RenderCopy(ren, g_mask, NULL, d);

    /* 4: glow */
    SDL_SetTextureBlendMode(g_quarter, SDL_BLENDMODE_ADD);
    SDL_SetTextureAlphaMod(g_quarter, (Uint8)K->glow);
    SDL_RenderCopy(ren, g_quarter, NULL, d);
    SDL_SetTextureAlphaMod(g_quarter, 255);

    SDL_SetTextureScaleMode(game, SDL_ScaleModeNearest);
    return 1;
}
