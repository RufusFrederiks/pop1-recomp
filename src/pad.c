/* pad.c - game controllers (Xbox, PlayStation, Switch Pro and anything else
 * SDL knows), through SDL2's GameController API.
 *
 * The game never learns a controller is there. PoP reads its controls with
 * GetKeys (input.c), so a held button simply switches on the KeyMap bit of
 * the key it stands for: the D-pad and left stick are the arrow keys, A is
 * Shift, and so on. Keyboard and controllers work at the same time, and every
 * connected controller counts (their buttons are OR'd together).
 *
 *  - Buttons by position, not label: SDL's "A" is the bottom face button on
 *    every controller (Cross on a PlayStation pad, B on a Switch Pro).
 *  - Hot-plugging: controllers are looked for whenever the number of
 *    joysticks changes, so one can be plugged in or out mid-game.
 *  - The recomp menu: Start opens and closes it; in it the D-pad or stick
 *    moves, A selects, B goes back. Buttons held when the menu closes are
 *    ignored until let go, so the A that picked "Resume" does not also grab.
 *  - Save and load state are combinations: hold View (Back), press RB to
 *    save or LB to load. On the shoulders alone they went off by mistake.
 *    View tapped on its own is the next look.
 *  - controller.cfg (next to pop1.cfg) says which button does what; it is
 *    written with the defaults the first time. A gamecontrollerdb.txt next
 *    to it adds mappings for controllers SDL does not know.
 *  - Rumble: pop_pad_rumble(), for the game layer to call.
 *
 * POPPAD scripts a virtual controller for headless runs, like POPKEYS:
 *   POPPAD="start@3,a@4,right@8+2,a@10+0.5,lx=-1@12+1"
 * name@t presses a button at t seconds for 0.1 s, name@t+d for d seconds;
 * lx/ly/lt/rt=v@t+d hold an axis at v (-1..1). Button names as in
 * controller.cfg. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include <SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mac key codes the actions stand for (see input.c). */
enum { MK_RETURN = 0x24, MK_SHIFT = 0x38, MK_LEFT = 0x7B, MK_RIGHT = 0x7C, MK_DOWN = 0x7D, MK_UP = 0x7E };

/* ---- what a button can do ---- */
enum { A_NONE, A_LEFT, A_RIGHT, A_UP, A_DOWN, A_SHIFT, A_MENU, A_SAVE, A_LOAD, A_LOOK, A_HOTKEY, A_COUNT };
static const char *ACTION_NAME[A_COUNT] = { "none", "left", "right", "up", "down", "shift", "menu", "save", "load", "look", "hotkey" };
static const int ACTION_KEY[A_COUNT] = { -1, MK_LEFT, MK_RIGHT, MK_UP, MK_DOWN, MK_SHIFT, -1, -1, -1, -1, -1 };

/* ---- the inputs: SDL's buttons, then the two triggers as buttons ---- */
#define IN_LT SDL_CONTROLLER_BUTTON_MAX
#define IN_RT (SDL_CONTROLLER_BUTTON_MAX + 1)
#define NIN   (SDL_CONTROLLER_BUTTON_MAX + 2)
static const struct { const char *name; int in; } INPUTS[] = {
    {"a",SDL_CONTROLLER_BUTTON_A},{"b",SDL_CONTROLLER_BUTTON_B},{"x",SDL_CONTROLLER_BUTTON_X},{"y",SDL_CONTROLLER_BUTTON_Y},
    {"lb",SDL_CONTROLLER_BUTTON_LEFTSHOULDER},{"rb",SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},{"lt",IN_LT},{"rt",IN_RT},
    {"start",SDL_CONTROLLER_BUTTON_START},{"back",SDL_CONTROLLER_BUTTON_BACK},
    {"dpup",SDL_CONTROLLER_BUTTON_DPAD_UP},{"dpdown",SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {"dpleft",SDL_CONTROLLER_BUTTON_DPAD_LEFT},{"dpright",SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {"ls",SDL_CONTROLLER_BUTTON_LEFTSTICK},{"rs",SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {"misc",SDL_CONTROLLER_BUTTON_MISC1},{"paddle1",SDL_CONTROLLER_BUTTON_PADDLE1},
    {"paddle2",SDL_CONTROLLER_BUTTON_PADDLE2},{"paddle3",SDL_CONTROLLER_BUTTON_PADDLE3},
    {"paddle4",SDL_CONTROLLER_BUTTON_PADDLE4},{"touchpad",SDL_CONTROLLER_BUTTON_TOUCHPAD},
};
#define NINPUTS ((int)(sizeof INPUTS / sizeof *INPUTS))

/* Settings (controller.cfg). Defaults: A grabs, B jumps, X grabs, Y crouches,
 * RT grabs, Start is the menu, Back is the hotkey: held with RB it saves,
 * with LB it loads, tapped alone it is the next look.
 * g_combo: what a button does while the hotkey is held (menu, save, load,
 * look; none = its own action). */
#define CFG_VERSION 2
static int g_bind[NIN], g_combo[NIN];
static int g_stick = 1;            /* left stick moves too */
static int g_dead = 50;            /* stick and trigger threshold, percent */
static int g_rumble = 1;
static void bind_defaults(void){
    memset(g_bind, 0, sizeof g_bind); memset(g_combo, 0, sizeof g_combo);
    g_bind[SDL_CONTROLLER_BUTTON_A] = A_SHIFT;  g_bind[SDL_CONTROLLER_BUTTON_B] = A_UP;
    g_bind[SDL_CONTROLLER_BUTTON_X] = A_SHIFT;  g_bind[SDL_CONTROLLER_BUTTON_Y] = A_DOWN;
    g_bind[IN_RT] = A_SHIFT;
    g_bind[SDL_CONTROLLER_BUTTON_START] = A_MENU; g_bind[SDL_CONTROLLER_BUTTON_BACK] = A_HOTKEY;
    g_combo[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = A_SAVE; g_combo[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = A_LOAD;
    g_bind[SDL_CONTROLLER_BUTTON_DPAD_UP] = A_UP;     g_bind[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = A_DOWN;
    g_bind[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = A_LEFT; g_bind[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = A_RIGHT;
}

static void cfg_path(char *out, size_t n, const char *file){ snprintf(out, n, "%s/../%s", pop_work_dir(), file); }

static void cfg_write(const char *p){
    FILE *f = fopen(p, "wb"); if(!f) return;
    fprintf(f, "# Prince of Persia recomp: controller buttons.\n"
               "# Buttons go by position, Xbox names: a = bottom, b = right, x = left, y = top\n"
               "# (PlayStation: a = Cross, b = Circle, x = Square, y = Triangle;\n"
               "#  Switch Pro: a = B, b = A, x = Y, y = X). lb/rb = shoulders, lt/rt = triggers,\n"
               "# start, back, dpup/dpdown/dpleft/dpright, ls/rs (stick clicks), misc, paddle1-4, touchpad.\n"
               "# What a button can do: left right up down shift (the game's keys),\n"
               "#   menu (the recomp menu), save / load (savestate, current slot; on a\n"
               "#   button of its own, load needs it held half a second), look (the next\n"
               "#   display look), hotkey (see below), none.\n"
               "# hotkey: hold it and press another button for that button's hotkey+ action\n"
               "#   (menu, save, load or look); tapped on its own it is the next look.\n"
               "#   E.g. hotkey+rb = save: hold back, press rb.\n"
               "# In the menu: D-pad or stick to move, a to select, b to go back, whatever the settings here.\n"
               "# stick: 1 = the left stick moves the Prince too. deadzone: how far (percent)\n"
               "# a stick or trigger must go to count. rumble: 1 = on.\n");
    for(int i = 0; i < NINPUTS; i++)
        fprintf(f, "%s = %s\n", INPUTS[i].name, ACTION_NAME[g_bind[INPUTS[i].in]]);
    for(int i = 0; i < NINPUTS; i++)
        if(g_combo[INPUTS[i].in]) fprintf(f, "hotkey+%s = %s\n", INPUTS[i].name, ACTION_NAME[g_combo[INPUTS[i].in]]);
    fprintf(f, "stick = %d\ndeadzone = %d\nrumble = %d\nversion = %d\n", g_stick, g_dead, g_rumble, CFG_VERSION);
    fclose(f);
}

static void cfg_load(void){
    bind_defaults();
    char p[1200]; cfg_path(p, sizeof p, "controller.cfg");
    FILE *f = fopen(p, "rb");
    if(!f){ cfg_write(p); return; }
    char line[160]; int version = 1;
    while(fgets(line, sizeof line, f)){
        char k[32], v[32];
        if(line[0] == '#' || sscanf(line, " %31[a-z0-9+] = %31[a-z0-9]", k, v) != 2) continue;
        if(!strcmp(k, "version")){ version = atoi(v); continue; }
        if(!strcmp(k, "stick")){ g_stick = atoi(v) != 0; continue; }
        if(!strcmp(k, "rumble")){ g_rumble = atoi(v) != 0; continue; }
        if(!strcmp(k, "deadzone")){ int d = atoi(v); if(d >= 5 && d <= 95) g_dead = d; continue; }
        int combo = !strncmp(k, "hotkey+", 7), in = -1, a = -1;
        for(int i = 0; i < NINPUTS; i++) if(!strcmp(k + (combo ? 7 : 0), INPUTS[i].name)) in = INPUTS[i].in;
        for(int i = 0; i < A_COUNT; i++) if(!strcmp(v, ACTION_NAME[i])) a = i;
        if(combo && a >= 0 && a != A_NONE && (a < A_MENU || a > A_LOOK)) a = -1;   /* combos: front-end actions only */
        if(in >= 0 && a >= 0) (combo ? g_combo : g_bind)[in] = a;
        else fprintf(stderr, "[pad] controller.cfg: '%s = %s' not understood\n", k, v);
    }
    fclose(f);
    /* The first version had save and load on RB and LB alone, too easy to
     * hit by mistake. A file still on those defaults gets the new ones. */
    if(version < CFG_VERSION){
        if(g_bind[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] == A_SAVE && g_bind[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] == A_LOAD
           && g_bind[SDL_CONTROLLER_BUTTON_BACK] == A_LOOK){
            g_bind[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = g_bind[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = A_NONE;
            g_bind[SDL_CONTROLLER_BUTTON_BACK] = A_HOTKEY;
            fprintf(stderr, "[pad] controller.cfg: save/load moved to Back+RB / Back+LB\n");
        }
        cfg_write(p);
    }
}

/* ---- the open controllers ---- */
#define MAXPADS 4
static SDL_GameController *g_pad[MAXPADS];
static SDL_JoystickID g_pad_id[MAXPADS];
static int g_npads, g_seen_joysticks = -1, g_inited;
static SDL_GameControllerType g_type = SDL_CONTROLLER_TYPE_UNKNOWN;   /* the newest one's, for labels */

static void toast_upper(const char *pre, const char *name){
    char m[96]; snprintf(m, sizeof m, "%s%s", pre, name ? name : "CONTROLLER");
    for(char *c = m; *c; c++) *c = (char)toupper((unsigned char)*c);
    if(strlen(m) > 60) m[60] = 0;
    pop_toast(m);
}

static void rescan(void){
    for(int i = 0; i < g_npads; ){             /* gone ones */
        if(SDL_GameControllerGetAttached(g_pad[i])){ i++; continue; }
        toast_upper("DISCONNECTED: ", SDL_GameControllerName(g_pad[i]));
        SDL_GameControllerClose(g_pad[i]);
        g_pad[i] = g_pad[g_npads - 1]; g_pad_id[i] = g_pad_id[g_npads - 1]; g_npads--;
    }
    int n = SDL_NumJoysticks();
    for(int j = 0; j < n && g_npads < MAXPADS; j++){
        if(!SDL_IsGameController(j)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        int have = 0; for(int i = 0; i < g_npads; i++) have |= g_pad_id[i] == id;
        if(have) continue;
        SDL_GameController *gc = SDL_GameControllerOpen(j);
        if(!gc){ fprintf(stderr, "[pad] cannot open %d: %s\n", j, SDL_GetError()); continue; }
        g_pad[g_npads] = gc; g_pad_id[g_npads] = id; g_npads++;
        g_type = SDL_GameControllerGetType(gc);
        fprintf(stderr, "[pad] connected: %s (type %d)\n", SDL_GameControllerName(gc), (int)g_type);
        if(SDL_GameControllerHasLED(gc)) SDL_GameControllerSetLED(gc, 0xE8, 0xB8, 0x4A);   /* the menu's gold */
        toast_upper("CONTROLLER: ", SDL_GameControllerName(gc));
    }
    g_seen_joysticks = n;
}

/* ---- POPPAD: a scripted virtual controller ---- */
typedef struct { int in, axis; double v, t, dur; } PadScript;
static PadScript g_ps[256]; static int g_nps;
static int g_virt = -1;                    /* the virtual joystick's device index */
static SDL_Joystick *g_vjoy;

static void script_init(void){
    const char *e = getenv("POPPAD");
    if(!e) return;
    SDL_VirtualJoystickDesc d; memset(&d, 0, sizeof d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX; d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = "Scripted Controller";
    g_virt = SDL_JoystickAttachVirtualEx(&d);
    if(g_virt < 0){ fprintf(stderr, "[pad] virtual controller: %s\n", SDL_GetError()); return; }
    g_vjoy = SDL_JoystickOpen(g_virt);
    static char buf[8192]; snprintf(buf, sizeof buf, "%s", e);
    for(char *tok = strtok(buf, ","); tok && g_nps < 256; tok = strtok(NULL, ",")){
        PadScript *s = &g_ps[g_nps]; memset(s, 0, sizeof *s); s->in = -1; s->axis = -1; s->v = 1;
        char *at = strchr(tok, '@'); if(!at) continue; *at++ = 0;
        char *eq = strchr(tok, '='); if(eq){ *eq++ = 0; s->v = atof(eq); }
        static const struct { const char *n; int a; } AX[] = { {"lx",SDL_CONTROLLER_AXIS_LEFTX},{"ly",SDL_CONTROLLER_AXIS_LEFTY},
            {"rx",SDL_CONTROLLER_AXIS_RIGHTX},{"ry",SDL_CONTROLLER_AXIS_RIGHTY},
            {"lt",SDL_CONTROLLER_AXIS_TRIGGERLEFT},{"rt",SDL_CONTROLLER_AXIS_TRIGGERRIGHT} };
        for(unsigned i = 0; i < sizeof AX / sizeof *AX; i++) if(!strcmp(tok, AX[i].n)) s->axis = AX[i].a;
        if(s->axis < 0) for(int i = 0; i < NINPUTS; i++) if(!strcmp(tok, INPUTS[i].name) && INPUTS[i].in < SDL_CONTROLLER_BUTTON_MAX) s->in = INPUTS[i].in;
        if(s->axis < 0 && s->in < 0){ fprintf(stderr, "[pad] POPPAD: unknown '%s'\n", tok); continue; }
        char *plus = strchr(at, '+');
        s->t = atof(at); s->dur = plus ? atof(plus + 1) : 0.1;
        g_nps++;
    }
    fprintf(stderr, "[pad] scripted controller with %d step(s)\n", g_nps);
}

static void script_tick(void){
    if(!g_vjoy) return;
    double now = pop_wall_ticks() / 60.0;
    int btn[SDL_CONTROLLER_BUTTON_MAX] = {0}; double ax[SDL_CONTROLLER_AXIS_MAX] = {0};
    for(int i = 0; i < g_nps; i++){
        PadScript *s = &g_ps[i];
        if(now < s->t || now >= s->t + s->dur) continue;
        if(s->in >= 0) btn[s->in] = 1; else ax[s->axis] = s->v;
    }
    for(int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) SDL_JoystickSetVirtualButton(g_vjoy, b, (Uint8)btn[b]);
    for(int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++){
        double v = ax[a] < -1 ? -1 : ax[a] > 1 ? 1 : ax[a];
        SDL_JoystickSetVirtualAxis(g_vjoy, a, (Sint16)(v < 0 ? v * 32768 : v * 32767));
    }
}

/* ---- init ---- */
static void pad_init(void){
    g_inited = 1;
    cfg_load();
    /* Bottom button = A on every controller (by position, not by the
     * letters printed on a Nintendo pad). Must be set before opening. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0){
        fprintf(stderr, "[pad] no controller support: %s\n", SDL_GetError());
        g_inited = 2; return;
    }
    char p[1200]; cfg_path(p, sizeof p, "gamecontrollerdb.txt");
    int n = SDL_GameControllerAddMappingsFromFile(p);
    if(n > 0) fprintf(stderr, "[pad] %d mapping(s) from gamecontrollerdb.txt\n", n);
    script_init();
}

/* ---- reading them ---- */
static int g_held[NIN], g_prev[NIN];       /* inputs held now / at the last poll */
static int g_sx, g_sy, g_psx, g_psy;       /* the stick as -1/0/1, now and last poll */
static int g_latch;                        /* game keys off until everything is let go */
static uint32_t g_load_since;              /* when the load button went down (ms), 0 = not held */
static int g_hot_used;                     /* the hotkey was used for a combination while held */
static uint32_t g_rep_at; static int g_rep_dir;  /* menu auto-repeat */

static void read_pads(void){
    memcpy(g_prev, g_held, sizeof g_held); g_psx = g_sx; g_psy = g_sy;
    memset(g_held, 0, sizeof g_held); g_sx = g_sy = 0;
    int th = g_dead * 32767 / 100;
    for(int i = 0; i < g_npads; i++){
        SDL_GameController *gc = g_pad[i];
        for(int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) if(SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) g_held[b] = 1;
        if(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > th) g_held[IN_LT] = 1;
        if(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > th) g_held[IN_RT] = 1;
        int x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if(x < -th) g_sx = -1; else if(x > th) g_sx = 1;
        if(y < -th) g_sy = -1; else if(y > th) g_sy = 1;
    }
}

static int pressed(int in){ return g_held[in] && !g_prev[in]; }

/* A key for the front-end (platform.c's pop_ui_key), as if typed. */
static void ui_key(SDL_Keycode k){
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
    pop_ui_key(&e);
}

/* The menu: D-pad / stick move (with auto-repeat), A selects, B and Start
 * go back. */
static void menu_input(void){
    int dir = 0;                               /* 1 up 2 down 3 left 4 right */
    if(g_held[SDL_CONTROLLER_BUTTON_DPAD_UP] || (g_stick && g_sy < 0)) dir = 1;
    else if(g_held[SDL_CONTROLLER_BUTTON_DPAD_DOWN] || (g_stick && g_sy > 0)) dir = 2;
    else if(g_held[SDL_CONTROLLER_BUTTON_DPAD_LEFT] || (g_stick && g_sx < 0)) dir = 3;
    else if(g_held[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] || (g_stick && g_sx > 0)) dir = 4;
    uint32_t now = SDL_GetTicks();
    if(dir != g_rep_dir){ g_rep_dir = dir; g_rep_at = now + 400; if(dir) goto fire; }
    else if(dir && (dir <= 2) && now >= g_rep_at){ g_rep_at = now + 110; goto fire; }
    goto buttons;
fire:
    { static const SDL_Keycode K[] = { 0, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT }; ui_key(K[dir]); }
buttons:
    if(pressed(SDL_CONTROLLER_BUTTON_A)) ui_key(SDLK_RETURN);
    else if(pressed(SDL_CONTROLLER_BUTTON_B) || pressed(SDL_CONTROLLER_BUTTON_START)) ui_key(SDLK_ESCAPE);
}

static int hotkey_held(void){
    for(int in = 0; in < NIN; in++) if(g_held[in] && g_bind[in] == A_HOTKEY) return 1;
    return 0;
}

/* The game's keys from the controllers, as Mac key codes; KeyMap form.
 * With the hotkey held, buttons that have a combination give no key. */
static void game_keys(uint8_t map[16]){
    memset(map, 0, 16);
    if(g_latch) return;
    int hot = hotkey_held();
    for(int in = 0; in < NIN; in++){
        if(hot && g_combo[in]) continue;
        int k = g_held[in] ? ACTION_KEY[g_bind[in]] : -1;
        if(k >= 0) map[k >> 3] |= (uint8_t)(1 << (k & 7));
    }
    if(g_stick){
        if(g_sx < 0) map[MK_LEFT >> 3] |= 1 << (MK_LEFT & 7);
        if(g_sx > 0) map[MK_RIGHT >> 3] |= 1 << (MK_RIGHT & 7);
        if(g_sy < 0) map[MK_UP >> 3] |= 1 << (MK_UP & 7);
        if(g_sy > 0) map[MK_DOWN >> 3] |= 1 << (MK_DOWN & 7);
    }
}
static int any_game_input(void){
    for(int in = 0; in < NIN; in++) if(g_held[in] && ACTION_KEY[g_bind[in]] >= 0) return 1;
    return g_stick && (g_sx || g_sy);
}

static uint8_t g_map[16], g_prev_map[16];

void pop_pad_poll(void){
    if(!g_inited) pad_init();
    if(g_inited == 2) return;
    script_tick();
    /* Controller events are not used (state is read directly); keep them
     * from piling up in SDL's queue. Device changes show in the count. */
    SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_FINGERDOWN - 1);
    if(SDL_NumJoysticks() != g_seen_joysticks) rescan();
    else for(int i = 0; i < g_npads; i++) if(!SDL_GameControllerGetAttached(g_pad[i])){ rescan(); break; }
    if(!g_npads){ memset(g_held, 0, sizeof g_held); memset(g_map, 0, 16); return; }
    read_pads();

    if(pop_ui_menu_open()){
        g_latch = 1;
        menu_input();
        memset(g_map, 0, 16); memcpy(g_prev_map, g_map, 16);
        return;
    }
    if(g_latch && !any_game_input()) g_latch = 0;

    /* front-end buttons: hotkey combinations first, then buttons of their own */
    uint32_t now = SDL_GetTicks();
    int load_held = 0, hot = hotkey_held();
    for(int in = 0; in < NIN; in++){
        int a = g_bind[in];
        if(hot && g_combo[in]){
            if(!pressed(in)) continue;
            g_hot_used = 1; a = g_combo[in];
            if(a == A_MENU){ ui_key(SDLK_ESCAPE); g_latch = 1; return; }
            if(a == A_SAVE) ui_key(SDLK_F5);
            if(a == A_LOAD) ui_key(SDLK_F9);      /* a deliberate combination: no hold needed */
            if(a == A_LOOK) ui_key(SDLK_F2);
            continue;
        }
        if(a == A_HOTKEY){                      /* tapped alone: the next look */
            if(pressed(in)) g_hot_used = 0;
            else if(!g_held[in] && g_prev[in] && !g_hot_used) ui_key(SDLK_F2);
            continue;
        }
        if(a == A_LOAD && g_held[in]) load_held = 1;
        if(!pressed(in)) continue;
        if(a == A_MENU){ ui_key(SDLK_ESCAPE); g_latch = 1; return; }
        if(a == A_SAVE) ui_key(SDLK_F5);
        if(a == A_LOOK) ui_key(SDLK_F2);
    }
    if(!load_held) g_load_since = 0;
    else if(!g_load_since) g_load_since = now;
    else if(g_load_since != 1 && now - g_load_since >= 500){ g_load_since = 1; ui_key(SDLK_F9); }

    /* the game's keys, and key events for the ones that changed (as the
     * keyboard sends them; Shift alone sends none on a Mac). A dialog takes
     * A as Return. */
    game_keys(g_map);
    Rect dr; int dialog = pop_dialog_rect(&dr);
    for(int k = 0; k < 128; k++){
        int now_k = (g_map[k >> 3] >> (k & 7)) & 1, was = (g_prev_map[k >> 3] >> (k & 7)) & 1;
        if(now_k == was || k == MK_SHIFT) continue;
        pop_post_key(now_k ? 3 : 4, k);
    }
    if(dialog && pressed(SDL_CONTROLLER_BUTTON_A)){ pop_post_key(3, MK_RETURN); pop_post_key(4, MK_RETURN); }
    memcpy(g_prev_map, g_map, 16);
}

void pop_pad_keymap(uint8_t map[16]){
    if(pop_ui_menu_open()) return;
    for(int i = 0; i < 16; i++) map[i] |= g_map[i];
}
int pop_pad_shift(void){ return !pop_ui_menu_open() && ((g_map[MK_SHIFT >> 3] >> (MK_SHIFT & 7)) & 1); }

/* ---- rumble ---- */
void pop_pad_rumble(int low, int high, int ms){
    if(!g_rumble) return;
    for(int i = 0; i < g_npads; i++)
        SDL_GameControllerRumble(g_pad[i], (Uint16)(low * 65535 / 100), (Uint16)(high * 65535 / 100), (Uint32)ms);
}

/* The game plays sound effect n (resource 5000 + n; traps.c hooks
 * PlaySound): the ones you would feel shake the controller. Strength in
 * percent for the low (heavy) and high (light) motor, and milliseconds. */
void pop_pad_sound(int n){
    static int trace = -1; if(trace < 0) trace = getenv("POPPADSND") != 0;
    if(trace) fprintf(stderr, "[pad] sound %d (%d) at %.2f s\n", n, n + 5000, pop_wall_ticks() / 60.0);
    switch(n){
    case 13: pop_pad_rumble(70, 100, 300); break;   /* hit kid: the Prince is hurt */
    case 48: case 46: pop_pad_rumble(100, 100, 700); break;   /* impale (spikes), sliced (slicer) */
    case 0:  pop_pad_rumble(80, 40, 250); break;    /* land hard */
    case 16: pop_pad_rumble(35, 10, 120); break;    /* land med */
    case 2:  pop_pad_rumble(45, 20, 200); break;    /* crash: a loose floor hits the ground */
    case 14: pop_pad_rumble(40, 20, 250); break;    /* door slam */
    case 12: pop_pad_rumble(15, 45, 120); break;    /* hit guard */
    case 10: pop_pad_rumble(0, 30, 70); break;      /* sword on sword */
    default: break;
    }
}

/* ---- for the Controls page ---- */
int pop_pad_count(void){ return g_npads; }
const char *pop_pad_name(void){ return g_npads ? SDL_GameControllerName(g_pad[g_npads - 1]) : NULL; }

/* The name printed on an input, for the newest controller's family. */
static const char *label(int in){
    int ps = g_type == SDL_CONTROLLER_TYPE_PS3 || g_type == SDL_CONTROLLER_TYPE_PS4 || g_type == SDL_CONTROLLER_TYPE_PS5;
    int nin = g_type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO
#if SDL_VERSION_ATLEAST(2, 24, 0)
              || g_type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
#endif
              ;
    switch(in){
    case SDL_CONTROLLER_BUTTON_A: return ps ? "Cross" : nin ? "B" : "A";
    case SDL_CONTROLLER_BUTTON_B: return ps ? "Circle" : nin ? "A" : "B";
    case SDL_CONTROLLER_BUTTON_X: return ps ? "Square" : nin ? "Y" : "X";
    case SDL_CONTROLLER_BUTTON_Y: return ps ? "Triangle" : nin ? "X" : "Y";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ps ? "L1" : nin ? "L" : "LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ps ? "R1" : nin ? "R" : "RB";
    case IN_LT: return ps ? "L2" : nin ? "ZL" : "LT";
    case IN_RT: return ps ? "R2" : nin ? "ZR" : "RT";
    case SDL_CONTROLLER_BUTTON_START: return ps ? "Options" : nin ? "+" : "Menu";
    case SDL_CONTROLLER_BUTTON_BACK: return ps ? (g_type == SDL_CONTROLLER_TYPE_PS5 ? "Create" : "Share") : nin ? "-" : "View";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ps ? "L3" : "LS";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ps ? "R3" : "RS";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad Right";
    case SDL_CONTROLLER_BUTTON_MISC1: return nin ? "Capture" : "Share";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad";
    default: return "Paddle";
    }
}

/* The Controls page rows for the controllers: what each action is on. */
int pop_pad_rows(char rows[][2][48], int max){
    static const struct { int a; const char *what; } SHOW[] = {
        { A_SHIFT, "Grab, hold, sword" }, { A_UP, "Jump / climb" }, { A_DOWN, "Crouch / step down" },
        { A_MENU, "This menu" }, { A_SAVE, "Save state" }, { A_LOAD, "Load state" }, { A_LOOK, "Next look" } };
    int hotkey = -1;
    for(int i = 0; i < NINPUTS && hotkey < 0; i++) if(g_bind[INPUTS[i].in] == A_HOTKEY) hotkey = INPUTS[i].in;
    int n = 0;
    if(n < max){ snprintf(rows[n][0], 48, "Run / turn"); snprintf(rows[n][1], 48, g_stick ? "D-pad / left stick" : "D-pad"); n++; }
    for(unsigned s = 0; s < sizeof SHOW / sizeof *SHOW && n < max; s++){
        char buf[48] = ""; int dpad = 0;
        for(int i = 0; i < NINPUTS; i++){
            int in = INPUTS[i].in;
            if(g_bind[in] != SHOW[s].a) continue;
            if(in >= SDL_CONTROLLER_BUTTON_DPAD_UP && in <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT){ dpad = 1; continue; }
            if(strlen(buf) + strlen(label(in)) + 10 >= sizeof buf) break;
            if(buf[0]) strcat(buf, " / ");
            strcat(buf, label(in));
            if(SHOW[s].a == A_LOAD) strcat(buf, " (hold)");
        }
        if(hotkey >= 0){                     /* combinations, and the hotkey tapped for the look */
            if(SHOW[s].a == A_LOOK && strlen(buf) + strlen(label(hotkey)) + 3 < sizeof buf){
                if(buf[0]) strcat(buf, " / ");
                strcat(buf, label(hotkey));
            }
            for(int i = 0; i < NINPUTS; i++){
                int in = INPUTS[i].in;
                if(g_combo[in] != SHOW[s].a) continue;
                if(strlen(buf) + strlen(label(hotkey)) + strlen(label(in)) + 6 >= sizeof buf) break;
                if(buf[0]) strcat(buf, " / ");
                strcat(buf, label(hotkey)); strcat(buf, " + "); strcat(buf, label(in));
            }
        }
        if(!buf[0] && dpad) snprintf(buf, sizeof buf, "D-pad");
        if(!buf[0]) continue;
        snprintf(rows[n][0], 48, "%s", SHOW[s].what); snprintf(rows[n][1], 48, "%s", buf); n++;
    }
    if(n < max){ snprintf(rows[n][0], 48, "Running jump: run, then jump"); rows[n][1][0] = 0; n++; }
    if(n < max){ snprintf(rows[n][0], 48, "In the menu: %s selects, %s goes back", label(SDL_CONTROLLER_BUTTON_A), label(SDL_CONTROLLER_BUTTON_B)); rows[n][1][0] = 0; n++; }
    if(n < max){ snprintf(rows[n][0], 48, "Buttons: edit controller.cfg"); rows[n][1][0] = 0; n++; }
    return n;
}
