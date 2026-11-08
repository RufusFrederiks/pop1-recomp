/* platform.c - the window, the display and the recomp menu.
 *
 * Replaces macrecomp's platform_sdl.c (same plat_* interface, which the HAL
 * calls) so that this recomp can have a front-end of its own:
 *
 *  - Whole frames only. macrecomp presented on every GetNextEvent *and*
 *    EventAvail. PoP calls EventAvail in the middle of its frame, between
 *    painting the play area black for a new room and copying the room in, so
 *    a black frame reached the screen at every room change. Here the window
 *    is presented when the game asks for its next event (pop_frame(), the top
 *    of its loop), and otherwise at most every 100 ms (dialogs, modal loops).
 *  - Two looks. Original: whole-number scaling, letterboxed, the Mac's own
 *    pixels. Remastered: the frame is scaled 4x (nearest neighbour, on the
 *    GPU) into an intermediate texture, and that is smoothly scaled to the
 *    largest 4:3 picture that fits ("sharp bilinear"): it fills the screen at
 *    any size and stays crisp. Later filters (AdvMAME, HQ4x) go into the same
 *    4x stage; the menu is drawn after the filter so its text is never
 *    smoothed.
 *  - DPI-aware on Windows, so fullscreen uses the real screen resolution
 *    and not the smaller one Windows reports at 125%/150% display scaling.
 *  - Window sizes worked out from the screen: "Auto" (the default) is the
 *    largest that fits; the fixed sizes offered are only those that fit.
 *    Fullscreen (F11 or Alt+Enter), colour themes for the 1-bit picture,
 *    optional scanlines.
 *  - The recomp menu (Esc or F1, or Start on a controller; open at start-up): start or resume the game,
 *    restart the level, sound and music, display settings, controls, quit.
 *    It is drawn over the game in the game's own "Persian" font (FONT 25616)
 *    and pauses the game while it is open. Settings live in pop1.cfg next to
 *    the work/ folder.
 *
 * Game commands from the menu go in as the game's own menu choices
 * (pop_inject_menu), exactly as if picked from the Mac menu bar. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include "filters.h"
#include "crt.h"
#include "../ext/macrecomp/runtime/font5x7.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The picture: the 1-bit 512x342 screen, or in colour mode the 8-bit screen
 * (640x480 or 512x384) seen through the device's colour table. */
#define MAXW 640
#define MAXH 480
static int W = QD_W, H = QD_H;

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex, *g_ui;
static SDL_Texture *g_big;                 /* Remastered: the 4x stage (a render target) */
static SDL_Texture *g_ftex;                /* a CPU filter's 4x output, copied into the stage */
static uint32_t *g_fout, *g_prev;          /* that output, and the game frame it was made from */
static int g_prev_look = -1;               /* the filter g_fout holds; -1 = nothing yet */
static int g_ftexk;                        /* the factor g_ftex was made for */
static int g_bigk;                         /* its factor (3 or 4); 0 = not made yet, -1 = unavailable */
static uint32_t g_start_ms, g_last_present;
static uint32_t g_px[MAXW * MAXH], g_uipx[MAXW * MAXH];

/* ---- settings ---- */
typedef struct { const char *name; uint32_t ink, paper; } Theme;   /* ink = a set bit */
static const Theme THEMES[] = {
    { "Macintosh", 0xFF000000, 0xFFFFFFFF },
    { "Amber",     0xFF140B00, 0xFFFFB347 },
    { "Phosphor",  0xFF001208, 0xFF5CFF8A },
    { "Parchment", 0xFF2A1E12, 0xFFF1E4C6 },
    { "Midnight",  0xFFE8ECF4, 0xFF0A1028 },
};
#define NTHEMES ((int)(sizeof THEMES / sizeof *THEMES))
/* win: the window size, 0 = Auto (largest that fits), else in half steps of
 * the picture (2 = 1x, 3 = 1.5x, ...). look: an index into LOOKS. crop: leave
 * out the rows the colour game never draws in (see view_rect). */
static struct { int theme, win, fullscreen, scanlines, sound, music, graphics, look, crop; } S = { 0, 0, 0, 0, 1, 1, 1, 1, 1 };
static const char *GRAPHICS[] = { "B&W", "Colour", "LC Colour" };
/* The looks. Everything from index 1 on is drawn through the 4x stage;
 * the filter decides how the game frame gets to 4x (see filter_into_stage). */
enum { LOOK_ORIGINAL, LOOK_REMASTERED, LOOK_ADVMAME, LOOK_XBR, LOOK_CRT_MONITOR, LOOK_CRT_TV };
static const char *LOOKS[] = { "Original", "Remastered", "AdvMAME", "xBR", "CRT Monitor", "CRT TV" };
static const char *LOOK_HINT[] = { "WHOLE PIXELS, LIKE THE MAC", "SHARP PIXELS SCALED 4X TO FILL THE SCREEN",
                                   "SMOOTHED DIAGONALS, STILL PIXEL ART", "SMOOTH CURVES AND EDGES",
                                   "AN APPLE RGB MONITOR: FINE STRIPES, SOFT GLOW", "A TV: SCANLINES, SHADOW MASK, GLOW" };
#define IS_CRT(l) ((l) == LOOK_CRT_MONITOR || (l) == LOOK_CRT_TV)
#define NLOOKS ((int)(sizeof LOOKS / sizeof *LOOKS))
#define REMASTERED (S.look != LOOK_ORIGINAL)
#define MAXWIN 16
static int g_graphics_running = -1;        /* the mode this run started in */

static void cfg_path(char *out, size_t n){ snprintf(out, n, "%s/../pop1.cfg", pop_work_dir()); }
static void cfg_load(void){
    static int loaded; if(loaded) return; loaded = 1;
    char p[1200]; cfg_path(p, sizeof p);
    FILE *f = fopen(p, "rb"); if(!f) return;
    char line[128];
    while(fgets(line, sizeof line, f)){
        int v; char k[32];
        if(sscanf(line, "%31[a-z] = %d", k, &v) != 2) continue;
        if(!strcmp(k, "theme") && v >= 0 && v < NTHEMES) S.theme = v;
        else if(!strcmp(k, "window") && v >= 0 && v <= MAXWIN) S.win = v;   /* the old "scale" is ignored: Auto fits better */
        else if(!strcmp(k, "look") && v >= 0 && v < NLOOKS) S.look = v;
        else if(!strcmp(k, "crop")) S.crop = v != 0;
        else if(!strcmp(k, "fullscreen")) S.fullscreen = v != 0;
        else if(!strcmp(k, "scanlines")) S.scanlines = v != 0;
        else if(!strcmp(k, "graphics") && v >= 0 && v <= 2) S.graphics = v;
    }
    fclose(f);
}
static void cfg_save(void){
    char p[1200]; cfg_path(p, sizeof p);
    FILE *f = fopen(p, "wb"); if(!f) return;
    fprintf(f, "# Prince of Persia recomp settings\n# graphics: 0 black-and-white, 1 colour (640x480), 2 LC colour (512x384)\n"
               "# look: 0 original (whole-number scaling), 1 remastered (4x, fills the screen), 2 advmame, 3 xbr,\n"
               "#       4 crt monitor, 5 crt tv\n"
               "# crop: 1 = leave out the black rows the colour game never draws in\n"
               "# window: 0 auto (largest that fits), else the size in half steps (2 = 1x, 3 = 1.5x, ...)\n"
               "graphics = %d\nlook = %d\ncrop = %d\ntheme = %d\nwindow = %d\nfullscreen = %d\nscanlines = %d\n",
            S.graphics, S.look, S.crop, S.theme, S.win, S.fullscreen, S.scanlines);
    fclose(f);
}

/* The graphics mode this run uses: POPMODE (bw, colour/color, lc, or 0-2)
 * if set, else pop1.cfg, else colour. Asked once, before the machine is
 * built (pop_init): the game chooses its graphics set at start-up. */
int pop_graphics_mode(void){
    if(g_graphics_running >= 0) return g_graphics_running;
    cfg_load();
    int m = S.graphics;
    const char *e = getenv("POPMODE");
    if(e && *e){
        if(!strcmp(e, "bw") || !strcmp(e, "BW") || !strcmp(e, "0")) m = 0;
        else if(!strcmp(e, "lc") || !strcmp(e, "LC") || !strcmp(e, "2")) m = 2;
        else m = 1;
    }
    g_graphics_running = m;
    return m;
}

/* ---- the game's own font (Mac FONT resource) ---- */
static struct { const uint8_t *bits; int rowbytes, height, ascent, first, last, kern;
                const uint8_t *loc, *ow; int ok; } F;
static void font_load(void){
    int len; const uint8_t *b = pop_res("FONT", 25616, &len);
    if(!b || len < 26) return;
    #define BE16(p) ((int)(int16_t)((p)[0] << 8 | (p)[1]))
    F.first = BE16(b + 2); F.last = BE16(b + 4);
    F.kern = BE16(b + 8); F.height = BE16(b + 14); F.ascent = BE16(b + 18);
    int owTLoc = BE16(b + 16), rowWords = BE16(b + 24);
    F.rowbytes = rowWords * 2;
    F.bits = b + 26;
    F.loc = F.bits + F.rowbytes * F.height;
    F.ow = b + 16 + owTLoc * 2;              /* offset/width table: owTLoc words from its own field */
    int nchars = F.last - F.first + 3;
    F.ok = (F.loc + nchars * 2 <= b + len) && F.height > 0 && F.height < 64;
}
/* glyph c: image columns [x0,x1) of the strike, pen advance, left offset */
static int glyph(int c, int *x0, int *x1, int *adv, int *off){
    if(!F.ok || c < F.first || c > F.last) return 0;
    int i = c - F.first;
    const uint8_t *ow = F.ow + i * 2;
    if(ow[0] == 0xFF && ow[1] == 0xFF) return 0;      /* missing */
    *x0 = BE16(F.loc + i * 2); *x1 = BE16(F.loc + i * 2 + 2);
    *off = ow[0]; *adv = ow[1];
    return 1;
}
/* "Persian" is a capitals-only display face: lower case is drawn as upper
 * case, and anything it lacks (digits, most punctuation) in the 5x7 font at
 * double size. */
static int pchar(int c){ int a, b, adv, off; return glyph(c, &a, &b, &adv, &off) ? c :
                          (c >= 'a' && c <= 'z' && glyph(c - 32, &a, &b, &adv, &off)) ? c - 32 : -1; }
static int text_width(const char *s){
    int w = 0;
    for(; *s; s++){ int a, b, adv, off, c = pchar((unsigned char)*s);
        if(c >= 0 && glyph(c, &a, &b, &adv, &off)) w += adv; else w += 12; }
    return w;
}

/* ---- drawing into the UI layer (ARGB, blended over the game) ---- */
static void ui_clear(void){ memset(g_uipx, 0, sizeof g_uipx); }
static void ui_rect(int x, int y, int w, int h, uint32_t c){
    for(int j = y; j < y + h; j++) if(j >= 0 && j < H)
        for(int i = x; i < x + w; i++) if(i >= 0 && i < W) g_uipx[j * W + i] = c;
}
static void ui_hline(int x, int y, int w, uint32_t c){ ui_rect(x, y, w, 1, c); }
static void ui_vline(int x, int y, int h, uint32_t c){ ui_rect(x, y, 1, h, c); }
/* Text in the game's font; returns the width. y is the top of the line. */
static int ui_text(int x, int y, const char *s, uint32_t c){
    int x0 = x; uint32_t c2 = c;
    for(; *s; s++){
        int a, b, adv, off, ch = pchar((unsigned char)*s);
        if(ch < 0 || !glyph(ch, &a, &b, &adv, &off)){         /* 5x7 at 2x */
            int c = (unsigned char)*s;
            if(c >= FONT_FIRST && c <= FONT_LAST){
                const uint8_t *g = FONT5X7[c - FONT_FIRST];
                for(int col = 0; col < FONT_COLS; col++) for(int row = 0; row < 8; row++)
                    if((g[col] >> row) & 1) for(int dy = 0; dy < 2; dy++) for(int dx = 0; dx < 2; dx++){
                        int px = x + col * 2 + dx, py = y + 1 + row * 2 + dy;
                        if(px >= 0 && px < W && py >= 0 && py < H) g_uipx[py * W + px] = c2; }
            }
            x += 12; continue;
        }
        for(int row = 0; row < F.height; row++)
            for(int col = a; col < b; col++)
                if((F.bits[row * F.rowbytes + (col >> 3)] >> (7 - (col & 7))) & 1){
                    int px = x + F.kern + off + (col - a), py = y + row;
                    if(px >= 0 && px < W && py >= 0 && py < H) g_uipx[py * W + px] = c;
                }
        x += adv;
    }
    return x - x0;
}
static void ui_text_center(int cx, int y, const char *s, uint32_t c){ ui_text(cx - text_width(s) / 2, y, s, c); }
/* Small text in the 5x7 font. */
static int ui_small(int x, int y, const char *s, uint32_t c){
    int x0 = x;
    for(; *s; s++){
        int ch = (unsigned char)*s;
        if(ch >= FONT_FIRST && ch <= FONT_LAST){
            const uint8_t *g = FONT5X7[ch - FONT_FIRST];
            for(int col = 0; col < FONT_COLS; col++) for(int row = 0; row < 8; row++)
                if((g[col] >> row) & 1){ int px = x + col, py = y + row;
                    if(px >= 0 && px < W && py >= 0 && py < H) g_uipx[py * W + px] = c; }
        }
        x += 6;
    }
    return x - x0;
}
static void ui_small_center(int cx, int y, const char *s, uint32_t c){ ui_small(cx - (int)strlen(s) * 3, y, s, c); }

/* ---- the menu ---- */
/* M_SIZE is one row for Fullscreen / Window Auto / the window sizes that
 * fit. Display (the B&W themes) is only shown in B&W and Crop Borders only
 * in colour, so 13 rows are shown: the menu has to fit the 342-line B&W
 * screen, and the 320 rows of the cropped LC screen (rows shrink there). */
enum { M_GAME, M_RESTART, M_SAVE, M_LOAD, M_SOUND, M_MUSIC, M_GRAPHICS, M_LOOK, M_THEME, M_SIZE, M_CROP, M_SCAN, M_CONTROLS, M_QUIT, M_COUNT };
static void window_size(int *w, int *h);
static SDL_Rect g_view;                    /* the part of the picture shown (view_rect) */
static int g_menu_open, g_sel, g_page_controls, g_in_game, g_quit;
static int g_slot = 1;                     /* the savestate slot (1-9) */
static char g_toast[96]; static uint32_t g_toast_until;
static int item_visible(int i){ return !(i == M_THEME && g_graphics_running) && !(i == M_CROP && !g_graphics_running); }
static int n_visible(void){ int n = 0; for(int i = 0; i < M_COUNT; i++) n += item_visible(i); return n; }
/* Row height: 20, less if the shown part of the screen is short. */
static int row_h(void){ int r = (g_view.h - 78) / n_visible(); return r > 20 ? 20 : r < 16 ? 16 : r; }
#define ROW row_h()

int pop_ui_menu_open(void){ return g_menu_open; }

static const char *item_label(int i, char *buf, size_t n){
    switch(i){
    case M_GAME:     return g_in_game ? "Resume" : "Start Game";
    case M_RESTART:  return "Restart Level";
    case M_SAVE:     snprintf(buf, n, "Save State   Slot %d", g_slot); return buf;
    case M_LOAD:     snprintf(buf, n, "Load State   Slot %d", g_slot); return buf;
    case M_SOUND:    snprintf(buf, n, "Sound Effects   %s", S.sound ? "On" : "Off"); return buf;
    case M_MUSIC:    snprintf(buf, n, "Music   %s", S.music ? "On" : "Off"); return buf;
    case M_GRAPHICS: snprintf(buf, n, "Graphics   %s%s", GRAPHICS[S.graphics],
                              S.graphics != g_graphics_running ? " *" : ""); return buf;
    case M_THEME:    if(g_graphics_running) return "Display   Colour";
                     snprintf(buf, n, "Display   %s", THEMES[S.theme].name); return buf;
    case M_LOOK:     snprintf(buf, n, "Look   %s", LOOKS[S.look]); return buf;
    case M_CROP:     snprintf(buf, n, "Crop Borders   %s", S.crop ? "On" : "Off"); return buf;
    case M_SIZE:     if(S.fullscreen) return "Size   Fullscreen";
                     if(!S.win) return "Size   Window Auto";
                     { int w, h; window_size(&w, &h); snprintf(buf, n, "Size   %d x %d", w, h); } return buf;
    case M_SCAN:     if(IS_CRT(S.look)) return "Scanlines   Built In";
                     snprintf(buf, n, "Scanlines   %s", S.scanlines ? "On" : "Off"); return buf;
    case M_CONTROLS: return "Controls";
    case M_QUIT:     return "Quit";
    }
    return "";
}
static int item_enabled(int i){ return item_visible(i) && !((i == M_RESTART || i == M_SAVE) && !g_in_game)
                                       && !(i == M_SCAN && IS_CRT(S.look)); }

static const uint32_t GOLD = 0xFFE8B84A, GOLD_DIM = 0xFF8C6A22, CREAM = 0xFFF4E9D0,
                      GREY = 0xFF8A8478, PANEL = 0xE6100C08, SHADE = 0x99000000, SEL = 0xFF2A1F0E;

static void draw_frame(int x, int y, int w, int h){
    ui_rect(x, y, w, h, PANEL);
    ui_hline(x, y, w, GOLD); ui_hline(x, y + h - 1, w, GOLD);
    ui_vline(x, y, h, GOLD); ui_vline(x + w - 1, y, h, GOLD);
    ui_hline(x + 3, y + 3, w - 6, GOLD_DIM); ui_hline(x + 3, y + h - 4, w - 6, GOLD_DIM);
    ui_vline(x + 3, y + 3, h - 6, GOLD_DIM); ui_vline(x + w - 4, y + 3, h - 6, GOLD_DIM);
    /* corner jewels */
    int cx[4] = { x, x + w - 1, x, x + w - 1 }, cy[4] = { y, y, y + h - 1, y + h - 1 };
    for(int k = 0; k < 4; k++) for(int d = -3; d <= 3; d++){
        int e = 3 - (d < 0 ? -d : d);
        for(int q = -e; q <= e; q++){ int px = cx[k] + d, py = cy[k] + q;
            if(px >= 0 && px < W && py >= 0 && py < H) g_uipx[py * W + px] = GOLD; }
    }
}

static void draw_menu(void){
    ui_clear();
    ui_rect(0, 0, W, H, SHADE);
    int row = ROW, pw = 300, ph = 52 + row * n_visible() + 26, px = (W - pw) / 2, py = g_view.y + (g_view.h - ph) / 2;
    if(g_page_controls){ ph = 52 + 17 * 13 + 26; py = g_view.y + (g_view.h - ph) / 2; }
    draw_frame(px, py, pw, ph);
    ui_text_center(W / 2, py + 10, "Prince of Persia", GOLD);
    ui_small_center(W / 2, py + 33, "STATIC RECOMPILATION  -  68K MAC TO NATIVE", GREY);
    ui_hline(px + 30, py + 45, pw - 60, GOLD_DIM);
    if(g_page_controls == 2){                  /* the controller page */
        char rows[13][2][48]; int n = pop_pad_rows(rows, 13);
        const char *name = pop_pad_name();
        char head[64]; snprintf(head, sizeof head, "%s", name ? name : "No controller connected");
        for(char *c = head; *c; c++) if(*c >= 'a' && *c <= 'z') *c -= 32;
        if(strlen(head) > 46) head[46] = 0;
        ui_small_center(W / 2, py + 54, head, name ? GOLD : GREY);
        int y = py + 54 + 20;
        for(int i = 0; i < n; i++, y += 16){
            ui_small(px + 24, y, rows[i][0], rows[i][1][0] ? CREAM : GREY);
            if(rows[i][1][0]) ui_small(px + pw - 24 - (int)strlen(rows[i][1]) * 6, y, rows[i][1], GOLD);
        }
        ui_small_center(W / 2, py + ph - 18, "LEFT/RIGHT: KEYBOARD   ENTER OR ESC: BACK", GREY);
        return;
    }
    if(g_page_controls){
        static const char *rows[][2] = {
            { "Run / turn",        "Left / Right arrows" },
            { "Jump up / climb",   "Up arrow" },
            { "Crouch / step down","Down arrow" },
            { "Grab, hold, sword", "Shift" },
            { "Careful step",      "Shift + arrow" },
            { "Strike / block",    "Shift / Up (sword)" },
            { "Also: numeric keypad 7 8 9 4 6 1 2 3", "" },
            { "or letters  w e r / s d f, u i o / j k l", "" },
            { "New game",          "Ctrl+N" },
            { "This menu",         "Esc or F1" },
            { "Save / load state", "F5 / F9" },
            { "Next look",         "F2" },
            { "Fullscreen",        "F11 or Alt+Enter" },
        };
        int y = py + 56;
        for(unsigned i = 0; i < sizeof rows / sizeof *rows; i++, y += 17){
            ui_small(px + 24, y, rows[i][0], rows[i][1][0] ? CREAM : GREY);
            if(rows[i][1][0]) ui_small(px + pw - 24 - (int)strlen(rows[i][1]) * 6, y, rows[i][1], GOLD);
        }
        ui_small_center(W / 2, py + ph - 18, "LEFT/RIGHT: CONTROLLER   ENTER OR ESC: BACK", GREY);
        return;
    }
    int y = py + 52;
    for(int i = 0; i < M_COUNT; i++){
        if(!item_visible(i)) continue;
        char buf[64]; const char *l = item_label(i, buf, sizeof buf);
        uint32_t c = !item_enabled(i) ? 0xFF4A443A : (i == g_sel ? GOLD : CREAM);
        if(i == g_sel){
            ui_rect(px + 14, y - 1, pw - 28, row - 1, SEL);
            for(int k = 0; k < 6; k++) ui_vline(px + 22 + k, y + 4 + k, 12 - 2 * k, GOLD);  /* > */
        }
        ui_text_center(W / 2, y + (row - 20) / 2, l, c);
        y += row;
    }
    if(g_sel == M_GRAPHICS && S.graphics != g_graphics_running)
        ui_small_center(W / 2, py + ph - 18, "ENTER: RESTART THE GAME IN THIS MODE", GOLD);
    else if(g_sel == M_SAVE || g_sel == M_LOAD){
        char info[80], line[100]; pop_state_info(g_slot, info, sizeof info);
        snprintf(line, sizeof line, "SLOT %d: %s", g_slot, info);
        for(char *c = line; *c; c++) if(*c >= 'a' && *c <= 'z') *c -= 32;
        ui_small_center(W / 2, py + ph - 18, line, GOLD);
    }
    else if(g_sel == M_LOOK)
        ui_small_center(W / 2, py + ph - 18, LOOK_HINT[S.look], GOLD);
    else if(g_sel == M_CROP)
        ui_small_center(W / 2, py + ph - 18, S.crop ? "THE EMPTY BLACK ROWS LEFT OUT: A BIGGER PICTURE" : "THE WHOLE MAC SCREEN, BLACK ROWS INCLUDED", GOLD);
    else if(g_sel == M_SIZE)
        ui_small_center(W / 2, py + ph - 18, "F11 OR ALT+ENTER: FULLSCREEN", GOLD);
    else
        ui_small_center(W / 2, py + ph - 18, "UP/DOWN CHOOSE  LEFT/RIGHT CHANGE  ENTER SELECT", GREY);
}

/* ---- the window ---- */
/* The part of the picture that is shown. The colour game draws only in a
 * band of its 8-bit screen and leaves the rest black, in every scene
 * (measured over the intro, title, story and play): 640x480 -> rows 40-439,
 * LC 512x384 -> rows 25-344. "Crop Borders" shows only that band, so the
 * picture can be ~20% bigger. B&W uses its whole screen. While a dialog or
 * alert is up, the band is widened to take it in. */
static SDL_Rect view_rect(void){
    SDL_Rect v = { 0, 0, W, H };
    if(!S.crop || !g_graphics_running) return v;
    if(W == 640 && H == 480){ v.y = 40; v.h = 400; }
    else if(W == 512 && H == 384){ v.y = 25; v.h = 320; }
    else return v;
    Rect o;
    if(pop_dialog_rect(&o)){
        int top = o.top < v.y ? o.top : v.y, bot = o.bottom > v.y + v.h ? o.bottom : v.y + v.h;
        if(top < 0) top = 0;
        if(bot > H) bot = H;
        v.y = top; v.h = bot - top;
    }
    return v;
}

/* The room a window can have on its screen: the desktop minus the taskbar
 * and the window's own frame. */
static void usable_area(int *uw, int *uh){
    int di = g_win ? SDL_GetWindowDisplayIndex(g_win) : 0; if(di < 0) di = 0;
    SDL_Rect r;
    if(SDL_GetDisplayUsableBounds(di, &r) != 0 && SDL_GetDisplayBounds(di, &r) != 0){ r.w = W * 2; r.h = H * 2; }
    int top = 0, left = 0, bottom = 0, right = 0;
    if(!g_win || SDL_GetWindowBordersSize(g_win, &top, &left, &bottom, &right) != 0 || top == 0){
        top = 32; left = right = bottom = 8; }                       /* a typical title bar and frame */
    *uw = r.w - left - right; *uh = r.h - top - bottom;
    if(*uw < g_view.w) *uw = g_view.w;
    if(*uh < g_view.h) *uh = g_view.h;
}
/* The largest fixed size that fits, in half steps (2 = 1x). Original only
 * offers whole steps. */
static int max_halfsteps(void){
    int uw, uh; usable_area(&uw, &uh);
    int n = 2 * uw / g_view.w < 2 * uh / g_view.h ? 2 * uw / g_view.w : 2 * uh / g_view.h;
    if(!REMASTERED) n &= ~1;
    if(n > MAXWIN) n = MAXWIN;
    return n < 2 ? 2 : n;
}
/* The window size (its inside) for the current setting. */
static void window_size(int *w, int *h){
    if(!S.win){                              /* Auto: the largest that fits */
        int uw, uh; usable_area(&uw, &uh);
        double f = (double)uw / g_view.w < (double)uh / g_view.h ? (double)uw / g_view.w : (double)uh / g_view.h;
        if(!REMASTERED){ int s = (int)f; f = s < 1 ? 1 : s; }        /* whole steps */
        *w = (int)(g_view.w * f); *h = (int)(g_view.h * f);
        return;
    }
    int n = S.win, m = max_halfsteps();
    if(!REMASTERED) n &= ~1;
    if(n > m) n = m;
    if(n < 2) n = 2;
    *w = g_view.w * n / 2; *h = g_view.h * n / 2;
}
static void apply_window(void){
    if(!g_win) return;
    g_view = view_rect();
    SDL_SetWindowMinimumSize(g_win, g_view.w, g_view.h);
    SDL_SetWindowFullscreen(g_win, S.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if(!S.fullscreen){
        int w, h; window_size(&w, &h);
        SDL_SetWindowSize(g_win, w, h);
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}
/* The Size row: Fullscreen, Window Auto, then the fixed sizes that fit. */
static void size_step(int dir){
    int list[MAXWIN + 2], n = 0, cur = 0;
    list[n++] = -1; list[n++] = 0;
    for(int k = 2; k <= max_halfsteps(); k += REMASTERED ? 1 : 2) list[n++] = k;
    int now = S.fullscreen ? -1 : S.win;
    if(!S.fullscreen && now && !REMASTERED) now &= ~1;
    for(int i = 0; i < n; i++) if(list[i] == now) cur = i;
    cur = (cur + (dir ? dir : 1) + n) % n;
    if(list[cur] < 0) S.fullscreen = 1;
    else { S.fullscreen = 0; S.win = list[cur]; }
    apply_window();
}

int plat_open(const char *title, int scale){
    (void)scale; (void)title;
    cfg_load();
    font_load();
    {   uint32_t b; int w, h, rb;
        if(pop_colour_screen(&b, &w, &h, &rb)){ W = w; H = h; } }
    /* Windows: say we handle display scaling ourselves. Otherwise at 125% or
     * 150% Windows hands us a smaller pretend screen and stretches the
     * result, which made fullscreen small and blurry. (SDL 2.24+) */
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0){ fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");       /* crisp pixels */
    g_bigk = 0;
    /* Made hidden, sized to the screen it is on, then shown: no jump. */
    g_win = SDL_CreateWindow("Prince of Persia \xE2\x80\x94 recompiled", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, W, H,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if(!g_win){ fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    g_view = view_rect();
    SDL_SetWindowMinimumSize(g_win, g_view.w, g_view.h);
    if(!S.fullscreen){
        int w, h; window_size(&w, &h);
        SDL_SetWindowSize(g_win, w, h);
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    SDL_ShowWindow(g_win);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if(!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    if(!g_ren){ fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    g_ui  = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureBlendMode(g_ui, SDL_BLENDMODE_BLEND);
    g_start_ms = SDL_GetTicks();
    if(S.fullscreen) apply_window();
    g_menu_open = getenv("POPNOMENU") ? 0 : 1;              /* the start menu */
    return (g_tex && g_ui) ? 0 : 1;
}

/* Where the picture goes in the window. Original: the largest whole-number
 * scale. Remastered (or a window smaller than 1x): the largest that fits,
 * keeping the picture's shape. */
static SDL_Rect dest_rect(void){
    int ww, wh; SDL_GetRendererOutputSize(g_ren, &ww, &wh);
    int vw = g_view.w, vh = g_view.h;
    int s = ww / vw < wh / vh ? ww / vw : wh / vh;
    SDL_Rect d;
    if(s >= 1 && !REMASTERED){ d.w = vw * s; d.h = vh * s; }
    else { double f = (double)ww / vw < (double)wh / vh ? (double)ww / vw : (double)wh / vh;
           d.w = (int)(vw * f + 0.5); d.h = (int)(vh * f + 0.5); }
    d.x = (ww - d.w) / 2; d.y = (wh - d.h) / 2;
    return d;
}

/* Remastered: the stage, a render target scaled smoothly to the screen.
 * The game frame goes in at k times its size: nearest neighbour (each Mac
 * pixel a clean block) or a filter. Scaling that sharp image to the final
 * size with bilinear filtering gives even pixels at any size with only a
 * hairline of softening on their edges.
 * k: plain Remastered uses 4. The filters use the factor nearest to the
 * size on screen, 3 below 3.5x (a 1080p screen shows the game at ~2.5-2.7x)
 * and 4 above, so their work reaches the screen almost 1:1 instead of being
 * shrunk away. Made again when the wanted factor changes. */
static int stage_factor(const SDL_Rect *d){
    if(S.look == LOOK_REMASTERED || IS_CRT(S.look)) return 4;
    return (double)d->h / g_view.h < 3.5 ? 3 : 4;
}
static int big_stage(int want){
    if(g_bigk && (g_bigk < 0 || g_bigk == want)) return g_bigk > 0;
    if(g_big){ SDL_DestroyTexture(g_big); g_big = NULL; }
    g_bigk = -1;
    if(!SDL_RenderTargetSupported(g_ren)) return 0;
    SDL_RendererInfo ri; int maxw = 4096, maxh = 4096;
    if(SDL_GetRendererInfo(g_ren, &ri) == 0 && ri.max_texture_width){ maxw = ri.max_texture_width; maxh = ri.max_texture_height; }
    int k = want;
    while(k > 1 && (W * k > maxw || H * k > maxh)) k--;
    if(k < 2) return 0;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");       /* this one is scaled smoothly */
    g_big = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, W * k, H * k);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if(!g_big){ fprintf(stderr, "[display] no %dx stage: %s\n", k, SDL_GetError()); return 0; }
    g_bigk = k;
    return 1;
}

/* The filter step: the game frame into the 4x stage. Remastered (nearest
 * neighbour) is a plain copy of g_tex, which samples nearest. AdvMAME and xBR
 * run on the CPU into g_fout, but only over the rows that changed since the
 * last frame (plus the filter's reach), and only those rows are uploaded. */
static void filter_into_stage(void){
    int k = g_bigk;
    if(S.look == LOOK_REMASTERED || IS_CRT(S.look) || (k != 3 && k != 4)){ SDL_RenderCopy(g_ren, g_tex, NULL, NULL); return; }
    if(!g_fout){
        g_fout = malloc((size_t)MAXW * 4 * MAXH * 4 * 4);
        g_prev = malloc((size_t)MAXW * MAXH * 4);
        if(!g_fout || !g_prev){ free(g_fout); free(g_prev); g_fout = g_prev = NULL; g_bigk = -1; return; }
    }
    if(g_ftex && g_ftexk != k){ SDL_DestroyTexture(g_ftex); g_ftex = NULL; }
    if(!g_ftex){
        g_ftex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W * k, H * k);
        if(!g_ftex){ SDL_RenderCopy(g_ren, g_tex, NULL, NULL); return; }
        g_ftexk = k;
        g_prev_look = -1;
    }
    int y0 = 0, y1 = H;
    if(g_prev_look == S.look){                       /* the rows that changed */
        while(y0 < H && !memcmp(g_px + y0 * W, g_prev + y0 * W, (size_t)W * 4)) y0++;
        while(y1 > y0 && !memcmp(g_px + (y1 - 1) * W, g_prev + (y1 - 1) * W, (size_t)W * 4)) y1--;
    }
    if(y0 < y1){
        y0 = y0 - FILTER_REACH < 0 ? 0 : y0 - FILTER_REACH;
        y1 = y1 + FILTER_REACH > H ? H : y1 + FILTER_REACH;
        if(S.look == LOOK_ADVMAME) filter_advmame(g_px, W, H, g_fout, k, y0, y1);
        else                       filter_xbr(g_px, W, H, g_fout, k, y0, y1);
        SDL_Rect r = { 0, y0 * k, W * k, (y1 - y0) * k };
        SDL_UpdateTexture(g_ftex, &r, g_fout + (size_t)y0 * k * W * k, W * k * 4);
        memcpy(g_prev, g_px, (size_t)W * H * 4);
        g_prev_look = S.look;
    }
    SDL_RenderCopy(g_ren, g_ftex, NULL, NULL);
}

/* The CRT looks: the 4x stage holds the plain game, crt.c builds the CRT
 * picture on the window from it, and the menu and toasts go on top through
 * the same stage (scaled smoothly, as in the other looks) without the
 * effect, so they stay readable. */
static int crt_present(const SDL_Rect *d, int ui){
    int k = g_bigk;
    SDL_SetRenderTarget(g_ren, g_big);
    SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_SetRenderTarget(g_ren, NULL);
    if(!crt_draw(g_ren, S.look == LOOK_CRT_TV, g_tex, &g_view, g_big, k, d)) return 0;
    if(ui){
        SDL_SetRenderTarget(g_ren, g_big);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
        SDL_RenderClear(g_ren);
        SDL_SetTextureBlendMode(g_ui, SDL_BLENDMODE_NONE);
        SDL_RenderCopy(g_ren, g_ui, NULL, NULL);
        SDL_SetTextureBlendMode(g_ui, SDL_BLENDMODE_BLEND);
        SDL_SetRenderTarget(g_ren, NULL);
        SDL_Rect sv = { g_view.x * k, g_view.y * k, g_view.w * k, g_view.h * k };
        SDL_SetTextureBlendMode(g_big, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(g_ren, g_big, &sv, d);
        SDL_SetTextureBlendMode(g_big, SDL_BLENDMODE_NONE);
    }
    return 1;
}

static void present_now(void){
    if(!g_ren) return;
    g_last_present = SDL_GetTicks();
    g_view = view_rect();
    uint32_t base = mr_screen_base();
    const Theme *t = &THEMES[S.theme];
    uint32_t cb; int cw, ch, crb;
    if(pop_colour_screen(&cb, &cw, &ch, &crb)){
        uint32_t pal[256]; pop_colour_clut(pal);
        for(int y = 0; y < H; y++){
            const uint8_t *row = M.mem + cb + (uint32_t)y * (uint32_t)crb;
            for(int x = 0; x < W; x++) g_px[y * W + x] = pal[row[x]];
        }
        /* Dialogs and alerts are the HAL's, drawn on its 1-bit screen: show
         * that screen where one is open. */
        Rect o;
        if(pop_dialog_rect(&o)){
            for(int y = o.top; y < o.bottom; y++){
                if(y < 0 || y >= H || y >= QD_H) continue;
                const uint8_t *row = M.mem + base + (uint32_t)y * (QD_W / 8);
                for(int x = o.left; x < o.right; x++){
                    if(x < 0 || x >= W || x >= QD_W) continue;
                    g_px[y * W + x] = ((row[x >> 3] >> (7 - (x & 7))) & 1) ? 0xFF000000 : 0xFFFFFFFF;
                }
            }
        }
    } else
    for(int y = 0; y < H; y++){
        const uint8_t *row = M.mem + base + (uint32_t)y * (W / 8);
        for(int x = 0; x < W; x++) g_px[y * W + x] = ((row[x >> 3] >> (7 - (x & 7))) & 1) ? t->ink : t->paper;
    }
    SDL_UpdateTexture(g_tex, NULL, g_px, W * 4);
    int toast = g_toast[0] && SDL_GetTicks() < g_toast_until, ui = g_menu_open || toast;
    if(ui){
        if(g_menu_open) draw_menu(); else ui_clear();
        if(toast){
            int tw = (int)strlen(g_toast) * 6 + 24, tx = (W - tw) / 2, ty = g_view.y + 10;
            ui_rect(tx, ty, tw, 20, PANEL);
            ui_hline(tx, ty, tw, GOLD); ui_hline(tx, ty + 19, tw, GOLD);
            ui_vline(tx, ty, 20, GOLD); ui_vline(tx + tw - 1, ty, 20, GOLD);
            ui_small(tx + 12, ty + 6, g_toast, CREAM);
        }
        SDL_UpdateTexture(g_ui, NULL, g_uipx, W * 4);
    }
    SDL_Rect d = dest_rect();
    if(IS_CRT(S.look) && big_stage(4) && crt_present(&d, ui)){
        /* drawn */
    } else if(REMASTERED && big_stage(stage_factor(&d))){
        /* game -> filter -> 4x stage (+ scanlines, + menu unfiltered) -> screen */
        int k = g_bigk;
        SDL_SetRenderTarget(g_ren, g_big);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        filter_into_stage();
        if(S.scanlines && d.h >= g_view.h * 2){       /* the lower half of each line, darkened */
            static SDL_Rect r[MAXH];
            for(int y = 0; y < H; y++){ r[y].x = 0; r[y].y = y * k + k - k / 2; r[y].w = W * k; r[y].h = k / 2; }
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 70);
            SDL_RenderFillRects(g_ren, r, H);
        }
        if(ui) SDL_RenderCopy(g_ren, g_ui, NULL, NULL);
        SDL_SetRenderTarget(g_ren, NULL);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        SDL_Rect sv = { g_view.x * k, g_view.y * k, g_view.w * k, g_view.h * k };
        SDL_RenderCopy(g_ren, g_big, &sv, &d);
    } else {
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        SDL_RenderCopy(g_ren, g_tex, &g_view, &d);
        if(S.scanlines && d.h >= g_view.h * 2){
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 70);
            int s = d.h / g_view.h;
            for(int y = 0; y < g_view.h; y++){ int ly = d.y + y * s + s - 1; SDL_RenderDrawLine(g_ren, d.x, ly, d.x + d.w - 1, ly); }
        }
        if(ui) SDL_RenderCopy(g_ren, g_ui, &g_view, &d);
    }
    if(getenv("POPWINSHOT")){      /* the final window picture (looks, crop, scaling), for headless checks */
        static int done; const char *at = getenv("POPWINSHOTAT");
        if(!done && pop_wall_ticks() >= (uint32_t)(60 * (at ? atof(at) : 0))){ done = 1;
            int ow, oh; SDL_GetRendererOutputSize(g_ren, &ow, &oh);
            uint32_t *px = malloc((size_t)ow * oh * 4); FILE *f = fopen(getenv("POPWINSHOT"), "wb");
            if(px && f && SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, ow * 4) == 0){
                fprintf(f, "P6\n%d %d\n255\n", ow, oh);
                for(int i = 0; i < ow * oh; i++){ fputc(px[i] >> 16 & 255, f); fputc(px[i] >> 8 & 255, f); fputc(px[i] & 255, f); }
            }
            if(f) fclose(f);
            free(px); }
    }
    SDL_RenderPresent(g_ren);
    if(getenv("POPUISHOT")){       /* what the player sees (menu, dialogs, toasts), for headless checks */
        static int done; const char *at = getenv("POPUISHOTAT");
        if(!done && pop_wall_ticks() >= (uint32_t)(60 * (at ? atof(at) : 0))){ done = 1;
            FILE *f = fopen(getenv("POPUISHOT"), "wb");
            if(f){ fprintf(f, "P6\n%d %d\n255\n", W, H);
                for(int i = 0; i < W * H; i++){
                    uint32_t g = g_px[i], u = (g_menu_open || toast) ? g_uipx[i] : 0; int a = (int)(u >> 24);
                    for(int k = 16; k >= 0; k -= 8){
                        int cg = (int)((g >> k) & 255), cu = (int)((u >> k) & 255);
                        fputc((cu * a + cg * (255 - a)) / 255, f); }
                }
                fclose(f); }
        }
    }
}

/* macrecomp calls this from many places (every event call, dialogs): show
 * the frame only if nothing has been shown for a while. */
void plat_present(void){
    {   const char *e = getenv("POPEXIT");           /* headless runs end on time, modal loops too */
        if(e && pop_wall_ticks() >= (uint32_t)(atof(e) * 60)){ fprintf(stderr, "[pop] POPEXIT after %s s\n", e); plat_shutdown(); exit(0); } }
    if(SDL_GetTicks() - g_last_present >= 100 && !pop_update_owed()) present_now();
}

void pop_toast(const char *msg){
    snprintf(g_toast, sizeof g_toast, "%s", msg);
    g_toast_until = SDL_GetTicks() + 2500;
    fprintf(stderr, "[ui] %s\n", msg);
    present_now();
}

/* From the game's tick waits (traps.c): keep the screen moving. */
void pop_tick_present(void){
    if(SDL_GetTicks() - g_last_present >= 20 && !pop_update_owed()) present_now();
}

/* ---- menu actions ---- */
static void menu_activate(int i, int dir){
    switch(i){
    case M_GAME:
        g_menu_open = 0;
        if(!g_in_game){ g_in_game = 1; pop_inject_menu(1001, 1); }   /* File > New Game */
        break;
    case M_RESTART: if(g_in_game){ g_menu_open = 0; pop_inject_menu(1003, 7); } break;   /* Game > Restart Level */
    case M_SAVE: case M_LOAD:
        if(dir){ g_slot = (g_slot - 1 + dir + 9) % 9 + 1; break; }
        pop_state_request(i == M_SAVE ? 1 : 2, g_slot);
        g_menu_open = 0;                    /* done at the top of the game's next frame */
        break;
    case M_SOUND: S.sound = !S.sound; pop_inject_menu(1003, 2); break;   /* Game > Sounds */
    case M_MUSIC: S.music = !S.music; pop_inject_menu(1003, 3); break;   /* Game > Music  */
    case M_GRAPHICS:
        if(dir){ S.graphics = (S.graphics + dir + 3) % 3; cfg_save(); }
        else if(S.graphics != g_graphics_running){ cfg_save(); plat_shutdown(); pop_restart(); }
        break;
    case M_THEME: S.theme = (S.theme + (dir ? dir : 1) + NTHEMES) % NTHEMES; cfg_save(); break;
    case M_LOOK: S.look = (S.look + (dir ? dir : 1) + NLOOKS) % NLOOKS;
                 if(!S.fullscreen) apply_window();      /* Original only has whole-number sizes */
                 g_prev_look = -1;
                 cfg_save(); break;
    case M_SIZE: size_step(dir); cfg_save(); break;
    case M_CROP: S.crop = !S.crop; if(!S.fullscreen) apply_window(); else g_view = view_rect(); cfg_save(); break;
    case M_SCAN: S.scanlines = !S.scanlines; cfg_save(); break;
    case M_CONTROLS: g_page_controls = pop_pad_count() ? 2 : 1; break;   /* the controller page if one is in */
    case M_QUIT: g_quit = 1; break;
    }
}

/* Every SDL key event goes through here first. 1 = the front-end used it. */
int pop_ui_key(const void *ev){
    const SDL_Event *e = (const SDL_Event *)ev;
    if(e->type != SDL_KEYDOWN) return g_menu_open;     /* key-ups: swallowed while the menu is up */
    SDL_Keycode k = e->key.keysym.sym;
    if(k == SDLK_F11 || (k == SDLK_RETURN && (e->key.keysym.mod & KMOD_ALT))){
        S.fullscreen = !S.fullscreen; apply_window(); cfg_save(); return 1; }
    if(k == SDLK_F2){                                   /* the next look (Shift: the previous) */
        S.look = (S.look + ((e->key.keysym.mod & KMOD_SHIFT) ? -1 : 1) + NLOOKS) % NLOOKS;
        if(!S.fullscreen) apply_window();
        cfg_save();
        char msg[64]; snprintf(msg, sizeof msg, "LOOK: %s", LOOKS[S.look]);
        for(char *c = msg; *c; c++) if(*c >= 'a' && *c <= 'z') *c -= 32;
        pop_toast(msg);
        return 1;
    }
    if(k == SDLK_F5 && g_in_game){ pop_state_request(1, g_slot); g_menu_open = 0; return 1; }
    if(k == SDLK_F9){ pop_state_request(2, g_slot); g_menu_open = 0; return 1; }
    if(!g_menu_open){
        if(k == SDLK_ESCAPE || k == SDLK_F1){ g_menu_open = 1; g_page_controls = 0; g_sel = 0; present_now(); return 1; }
        return 0;
    }
    if(e->key.repeat && k != SDLK_UP && k != SDLK_DOWN) return 1;
    if(g_page_controls){
        if(k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_F1) g_page_controls = 0;
        else if(k == SDLK_LEFT || k == SDLK_RIGHT) g_page_controls = 3 - g_page_controls;
        present_now(); return 1;
    }
    switch(k){
    case SDLK_UP:   do g_sel = (g_sel + M_COUNT - 1) % M_COUNT; while(!item_enabled(g_sel)); break;
    case SDLK_DOWN: do g_sel = (g_sel + 1) % M_COUNT; while(!item_enabled(g_sel)); break;
    case SDLK_LEFT: case SDLK_RIGHT:
        if(((g_sel >= M_SOUND && g_sel <= M_SCAN) || g_sel == M_SAVE || g_sel == M_LOAD) && item_enabled(g_sel))
            menu_activate(g_sel, k == SDLK_LEFT ? -1 : 1);
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: menu_activate(g_sel, 0); break;
    case SDLK_ESCAPE: case SDLK_F1: if(g_in_game) g_menu_open = 0; break;   /* at the start there is nothing to go back to */
    default: break;
    }
    present_now();
    return 1;
}

/* Called at the top of every game frame (GetNextEvent/WaitNextEvent): show
 * the frame, and while the menu is open in a game, hold the game here. */
void pop_frame(void){
    if(g_quit){ plat_shutdown(); exit(0); }
    /* Hold the last whole frame while the game owes itself an update (it has
     * blanked the play area for a new room), for at most three frames. */
    static int held;
    if(pop_update_owed() && !g_menu_open && held < 3) held++;
    else { held = 0; present_now(); }
    while(g_menu_open && g_in_game){
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type == SDL_QUIT){ plat_shutdown(); exit(0); }
            if(e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) pop_ui_key(&e);
            if(e.type == SDL_WINDOWEVENT) present_now();
        }
        pop_pad_poll();
        pop_script_poll();
        {   const char *e = getenv("POPEXIT");           /* headless runs end on time */
            if(e && pop_wall_ticks() >= (uint32_t)(atof(e) * 60)) g_quit = 1; }
        if(g_quit){ plat_shutdown(); exit(0); }
        present_now();
        SDL_Delay(16);
    }
}
void pop_note_new_game(void){ g_in_game = 1; g_menu_open = 0; }

/* ---- input plumbing the HAL uses ---- */
#define EVQ 64
static struct { int what, msg, h, v; } evq[EVQ];
static int evhead, evtail;
static int syn_x, syn_y, syn_down, syn_live;
static void evpush(int what, int msg, int h, int v){
    int n = (evtail + 1) % EVQ; if(n == evhead) return;
    evq[evtail].what = what; evq[evtail].msg = msg; evq[evtail].h = h; evq[evtail].v = v; evtail = n;
}
static void to_logical(int wx, int wy, int *h, int *v){
    SDL_Rect d = dest_rect();
    int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
    int ow, oh; SDL_GetRendererOutputSize(g_ren, &ow, &oh);
    double sx = ww ? (double)ow / ww : 1, sy = wh ? (double)oh / wh : 1;   /* high-DPI */
    *h = g_view.x + (d.w ? (int)((wx * sx - d.x) * g_view.w / d.w) : 0);
    *v = g_view.y + (d.h ? (int)((wy * sy - d.y) * g_view.h / d.h) : 0);
}

void plat_pump(void){
    SDL_Event e;
    {   Rect o; int what, msg;                 /* a dialog is up: scripted keys go to it */
        pop_pad_poll();                        /* controllers: A answers a dialog */
        if(pop_dialog_rect(&o)) while(pop_take_key(&what, &msg)) evpush(what, msg, 0, 0); }
    while(SDL_PollEvent(&e)){
        if(e.type == SDL_QUIT){ plat_shutdown(); exit(0); }
        else if(e.type == SDL_KEYDOWN || e.type == SDL_KEYUP){
            /* Normally src/input.c has already taken the keys; these are the
             * ones that arrive while macrecomp runs a modal loop (a dialog). */
            if(pop_ui_key(&e)) continue;
            if(e.type == SDL_KEYDOWN && !e.key.repeat){
                int msg = pop_mac_key_message(e.key.keysym.scancode);
                if(msg >= 0) evpush(3, msg, 0, 0);
            }
        }
        else if(g_menu_open) continue;
        else if(e.type == SDL_MOUSEBUTTONDOWN){ int h, v; to_logical(e.button.x, e.button.y, &h, &v); evpush(1, 0, h, v); }
        else if(e.type == SDL_MOUSEBUTTONUP){ int h, v; to_logical(e.button.x, e.button.y, &h, &v); evpush(2, 0, h, v); }
        else if(e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_EXPOSED) present_now();
    }
}
int plat_quit_requested(void){ return g_quit; }
void plat_get_mouse(int *h, int *v){
    if(syn_live){ if(h) *h = syn_x; if(v) *v = syn_y; return; }
    int x, y; SDL_GetMouseState(&x, &y);
    int lh = 0, lv = 0; if(g_ren) to_logical(x, y, &lh, &lv);
    if(h) *h = lh; if(v) *v = lv;
}
int plat_button(void){
    if(syn_live){ if(syn_down > 0 && !--syn_down){ evpush(2, 0, syn_x, syn_y); syn_live = 0; return 0; } return 1; }
    if(g_menu_open) return 0;
    return (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
}
/* The game's clock (Ticks, TickCount, VBL tasks): time since start-up, plus
 * an offset a loaded savestate sets so Ticks carries on from where it was
 * saved (never backwards, which the music driver would not survive). */
static int64_t g_tick_off;
uint32_t pop_wall_ticks(void){ return (SDL_GetTicks() - g_start_ms) * 60u / 1000u; }
uint32_t plat_ticks(void){ return (uint32_t)((int64_t)pop_wall_ticks() + g_tick_off); }
void pop_set_ticks(uint32_t t){ g_tick_off = (int64_t)t - (int64_t)pop_wall_ticks(); m68k_w32(0x16A, t); }
void plat_post_event(int what, int msg){ evpush(what, msg, syn_x, syn_y); }
void plat_inject_click(int x, int y){ syn_x = x; syn_y = y; syn_live = 1; syn_down = 30; evpush(1, 0, x, y); }
int plat_next_event(int *what, int *msg, int *h, int *v){
    if(evhead == evtail){ if(what) *what = 0; return 0; }
    if(what) *what = evq[evhead].what; if(msg) *msg = evq[evhead].msg;
    if(h) *h = evq[evhead].h; if(v) *v = evq[evhead].v;
    evhead = (evhead + 1) % EVQ; return 1;
}
void plat_shutdown(void){
    if(g_big) SDL_DestroyTexture(g_big);
    if(g_ftex) SDL_DestroyTexture(g_ftex);
    crt_free();
    g_big = g_ftex = NULL; g_bigk = 0; g_prev_look = -1;
    if(g_ui) SDL_DestroyTexture(g_ui);
    if(g_tex) SDL_DestroyTexture(g_tex);
    if(g_ren) SDL_DestroyRenderer(g_ren);
    if(g_win) SDL_DestroyWindow(g_win);
    g_ui = g_tex = NULL; g_ren = NULL; g_win = NULL;
    SDL_Quit();
}
