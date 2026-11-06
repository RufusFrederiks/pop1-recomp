/* input.c - the keyboard, as Prince of Persia reads it.
 *
 * PoP takes the keyboard two ways:
 *  - In play, the engine (CODE 3 0x0a3a) calls GetKeys and tests bits of the
 *    128-bit KeyMap: arrows, the numeric keypad, and two letter clusters
 *    (s/e/f/d with w/r, j/i/l/k with u/o), Shift for actions.
 *  - Everything else comes as keyDown events: Esc, and Command-keys, which
 *    go to MenuKey (File > New Game is Command-N).
 * At init it sets SysEvtMask to $FFDF, i.e. it wants keyUp events too.
 *
 * macrecomp's HAL reports an empty KeyMap and sends SDL key codes as event
 * messages, so this file takes the keyboard over: SDL key events are pulled
 * out of SDL's queue before macrecomp's pump sees them and turned into Mac
 * events (message = key code << 8 | character code), and GetKeys reads SDL's
 * keyboard state. A PC has no Command key, so Ctrl is Command and Alt is
 * Option.
 *
 * POPKEYS scripts the keyboard for headless runs, by wall-clock seconds:
 *   POPKEYS="^n@8,right@10+1.5,shift@12"
 * name@t taps a key at t seconds; name@t+d holds it for d seconds; a leading
 * ^ adds Command. Names: left right up down shift space return esc tab,
 * kp0..kp9, a single letter or digit, or a Mac key code like 0x7C. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include <SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mac virtual key codes (Inside Macintosh V-192, Apple Extended keyboard). */
enum { MK_RETURN = 0x24, MK_TAB = 0x30, MK_SPACE = 0x31, MK_DELETE = 0x33, MK_ESC = 0x35,
       MK_CMD = 0x37, MK_SHIFT = 0x38, MK_CAPS = 0x39, MK_OPTION = 0x3A, MK_CONTROL = 0x3B,
       MK_ENTER = 0x4C, MK_LEFT = 0x7B, MK_RIGHT = 0x7C, MK_DOWN = 0x7D, MK_UP = 0x7E };

static const char LETTER_CODES[26] = {   /* a..z */
    0x00,0x0B,0x08,0x02,0x0E,0x03,0x05,0x04,0x22,0x26,0x28,0x25,0x2E,
    0x2D,0x1F,0x23,0x0C,0x0F,0x01,0x11,0x20,0x09,0x0D,0x07,0x10,0x06 };
static const char DIGIT_CODES[10] = { 0x1D,0x12,0x13,0x14,0x15,0x17,0x16,0x1A,0x1C,0x19 };
static const char KEYPAD_CODES[10] = { 0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5B,0x5C };

static int mac_code(SDL_Scancode s){
    if(s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return LETTER_CODES[s - SDL_SCANCODE_A];
    if(s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return DIGIT_CODES[s - SDL_SCANCODE_1 + 1];
    if(s == SDL_SCANCODE_0) return DIGIT_CODES[0];
    if(s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return KEYPAD_CODES[s - SDL_SCANCODE_KP_1 + 1];
    switch(s){
    case SDL_SCANCODE_KP_0: return KEYPAD_CODES[0];
    case SDL_SCANCODE_KP_PERIOD: return 0x41;   case SDL_SCANCODE_KP_MULTIPLY: return 0x43;
    case SDL_SCANCODE_KP_PLUS: return 0x45;     case SDL_SCANCODE_KP_DIVIDE: return 0x4B;
    case SDL_SCANCODE_KP_ENTER: return MK_ENTER; case SDL_SCANCODE_KP_MINUS: return 0x4E;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x47; /* Clear */
    case SDL_SCANCODE_RETURN: return MK_RETURN; case SDL_SCANCODE_TAB: return MK_TAB;
    case SDL_SCANCODE_SPACE: return MK_SPACE;   case SDL_SCANCODE_BACKSPACE: return MK_DELETE;
    case SDL_SCANCODE_ESCAPE: return MK_ESC;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return MK_SHIFT;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return MK_OPTION;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return MK_CMD;
    case SDL_SCANCODE_CAPSLOCK: return MK_CAPS;
    case SDL_SCANCODE_LEFT: return MK_LEFT;     case SDL_SCANCODE_RIGHT: return MK_RIGHT;
    case SDL_SCANCODE_UP: return MK_UP;         case SDL_SCANCODE_DOWN: return MK_DOWN;
    case SDL_SCANCODE_MINUS: return 0x1B;       case SDL_SCANCODE_EQUALS: return 0x18;
    case SDL_SCANCODE_LEFTBRACKET: return 0x21; case SDL_SCANCODE_RIGHTBRACKET: return 0x1E;
    case SDL_SCANCODE_SEMICOLON: return 0x29;   case SDL_SCANCODE_APOSTROPHE: return 0x27;
    case SDL_SCANCODE_COMMA: return 0x2B;       case SDL_SCANCODE_PERIOD: return 0x2F;
    case SDL_SCANCODE_SLASH: return 0x2C;       case SDL_SCANCODE_BACKSLASH: return 0x2A;
    case SDL_SCANCODE_GRAVE: return 0x32;
    default: return -1;
    }
}

/* The character a key code types (US layout), for the event message. */
static int mac_char(int code, int shift){
    for(int i = 0; i < 26; i++) if(LETTER_CODES[i] == code) return (shift ? 'A' : 'a') + i;
    for(int i = 0; i < 10; i++) if(DIGIT_CODES[i] == code) return shift ? ")!@#$%^&*("[i] : '0' + i;
    for(int i = 0; i < 10; i++) if(KEYPAD_CODES[i] == code) return '0' + i;
    switch(code){
    case MK_RETURN: return 0x0D; case MK_ENTER: return 0x03; case MK_TAB: return 0x09;
    case MK_SPACE: return ' ';   case MK_DELETE: return 0x08; case MK_ESC: return 0x1B;
    case MK_LEFT: return 0x1C;   case MK_RIGHT: return 0x1D; case MK_UP: return 0x1E; case MK_DOWN: return 0x1F;
    case 0x41: return '.'; case 0x43: return '*'; case 0x45: return '+'; case 0x4B: return '/'; case 0x4E: return '-';
    case 0x1B: return shift ? '_' : '-'; case 0x18: return shift ? '+' : '=';
    case 0x2B: return shift ? '<' : ','; case 0x2F: return shift ? '>' : '.'; case 0x2C: return shift ? '?' : '/';
    default: return 0;
    }
}

/* ---- scripted keys (POPKEYS) ---- */
typedef struct { int code, cmd; double t, dur; int downsent, upsent; } Scripted;
#define MAX_SCRIPT 64
static Scripted g_script[MAX_SCRIPT];
static int g_nscript = -1;

static int code_by_name(const char *n){
    static const struct { const char *n; int c; } names[] = {
        {"left",MK_LEFT},{"right",MK_RIGHT},{"up",MK_UP},{"down",MK_DOWN},{"shift",MK_SHIFT},
        {"space",MK_SPACE},{"return",MK_RETURN},{"enter",MK_ENTER},{"esc",MK_ESC},{"tab",MK_TAB},
        {"option",MK_OPTION},{"cmd",MK_CMD} };
    for(unsigned i = 0; i < sizeof names / sizeof *names; i++) if(!strcmp(n, names[i].n)) return names[i].c;
    if(!strncmp(n, "kp", 2) && isdigit((unsigned char)n[2]) && !n[3]) return KEYPAD_CODES[n[2]-'0'];
    if(!strncmp(n, "0x", 2)) return (int)strtol(n, NULL, 16);
    if(n[0] && !n[1]){
        if(isalpha((unsigned char)n[0])) return LETTER_CODES[tolower((unsigned char)n[0]) - 'a'];
        if(isdigit((unsigned char)n[0])) return DIGIT_CODES[n[0]-'0'];
    }
    return -1;
}

static void script_load(void){
    g_nscript = 0;
    const char *e = getenv("POPKEYS");
    if(!e) return;
    char buf[2048]; snprintf(buf, sizeof buf, "%s", e);
    for(char *tok = strtok(buf, ","); tok && g_nscript < MAX_SCRIPT; tok = strtok(NULL, ",")){
        Scripted *s = &g_script[g_nscript];
        memset(s, 0, sizeof *s);
        if(*tok == '^'){ s->cmd = 1; tok++; }
        char *at = strchr(tok, '@'); if(!at) continue; *at++ = 0;
        s->code = code_by_name(tok);
        char *plus = strchr(at, '+');
        s->t = atof(at); s->dur = plus ? atof(plus + 1) : 0.1;
        if(s->code < 0){ fprintf(stderr, "[keys] unknown key '%s'\n", tok); continue; }
        g_nscript++;
    }
    fprintf(stderr, "[keys] %d scripted key(s)\n", g_nscript);
}

/* ---- the Mac event queue for keys ---- */
typedef struct { int what, msg, mods; } KeyEv;
#define KQ 64
static KeyEv g_kq[KQ];
static int g_kh, g_kt;
static void kq_push(int what, int code, int mods){
    int n = (g_kt + 1) % KQ; if(n == g_kh) return;
    int ch = mac_char(code, (mods & 0x0200) != 0);
    g_kq[g_kt].what = what; g_kq[g_kt].msg = (code << 8) | ch; g_kq[g_kt].mods = mods;
    g_kt = n;
}

static uint8_t g_held_script[16];

/* Modifiers word: cmdKey 0x0100, shiftKey 0x0200, alphaLock 0x0400,
 * optionKey 0x0800, controlKey 0x1000; btnState 0x0080 means the button is UP. */
static int modifiers(void){
    int m = 0;
    SDL_Keymod k = SDL_GetModState();
    if(k & (KMOD_CTRL | KMOD_GUI)) m |= 0x0100;
    if(k & KMOD_SHIFT) m |= 0x0200;
    if(k & KMOD_CAPS) m |= 0x0400;
    if(k & KMOD_ALT) m |= 0x0800;
    if(g_held_script[MK_SHIFT >> 3] & (1 << (MK_SHIFT & 7))) m |= 0x0200;
    if(g_held_script[MK_CMD >> 3] & (1 << (MK_CMD & 7))) m |= 0x0100;
    if(!plat_button()) m |= 0x0080;
    return m;
}

static void script_tick(void){
    if(g_nscript < 0) script_load();
    double now = plat_ticks() / 60.0;
    memset(g_held_script, 0, sizeof g_held_script);
    for(int i = 0; i < g_nscript; i++){
        Scripted *s = &g_script[i];
        if(now < s->t) continue;
        int held = now < s->t + s->dur;
        if(held){
            g_held_script[s->code >> 3] |= (uint8_t)(1 << (s->code & 7));
            if(s->cmd) g_held_script[MK_CMD >> 3] |= (uint8_t)(1 << (MK_CMD & 7));
        }
        int mods = (s->cmd ? 0x0100 : 0) | 0x0080;
        if(!s->downsent && pop_ui_menu_open()){   /* a scripted key meant for the menu */
            static const struct { int mac, sdl; } m[] = { {MK_RETURN, SDLK_RETURN}, {MK_UP, SDLK_UP},
                {MK_DOWN, SDLK_DOWN}, {MK_LEFT, SDLK_LEFT}, {MK_RIGHT, SDLK_RIGHT}, {MK_ESC, SDLK_ESCAPE},
                {MK_SPACE, SDLK_SPACE} };
            SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_KEYDOWN;
            for(unsigned k = 0; k < sizeof m / sizeof *m; k++) if(m[k].mac == s->code) e.key.keysym.sym = m[k].sdl;
            s->downsent = s->upsent = 1; s->dur = 0;
            fprintf(stderr, "[keys] 0x%02X to the menu at %.1f s\n", s->code, now);
            pop_ui_key(&e);
            continue;
        }
        if(!s->downsent && s->code == MK_ESC){    /* Esc opens the recomp menu, as on the keyboard */
            SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_KEYDOWN; e.key.keysym.sym = SDLK_ESCAPE;
            s->downsent = s->upsent = 1; pop_ui_key(&e); continue;
        }
        if(!s->downsent){ s->downsent = 1; kq_push(3, s->code, mods);
            fprintf(stderr, "[keys] %s0x%02X down at %.1f s\n", s->cmd ? "cmd-" : "", s->code, now); }
        if(!held && !s->upsent){ s->upsent = 1; kq_push(4, s->code, mods); }
    }
}

/* A modifier on its own makes no event on a Mac (it only shows up in the
 * modifiers word and the KeyMap). Sending one made a lone Ctrl press a
 * Command-key with character 0, and MenuKey(0) then matched the first menu
 * item without a key equivalent: About Prince of Persia. */
static int is_modifier(int code){
    return code == MK_SHIFT || code == MK_CMD || code == MK_OPTION || code == MK_CONTROL || code == MK_CAPS;
}

/* For the front-end's pause loop: scripted keys keep flowing while the game
 * is held. */
void pop_script_poll(void){ script_tick(); }

/* Take SDL's key events before macrecomp's pump does. The front-end
 * (platform.c: the recomp menu, fullscreen) sees each one first. */
static void pull_sdl_keys(void){
    SDL_Event ev[16]; int n;
    SDL_PumpEvents();
    while((n = SDL_PeepEvents(ev, 16, SDL_GETEVENT, SDL_KEYDOWN, SDL_KEYUP)) > 0){
        for(int i = 0; i < n; i++){
            if(pop_ui_key(&ev[i])) continue;
            if(ev[i].key.repeat) continue;   /* PoP masks autoKey out anyway */
            int code = mac_code(ev[i].key.keysym.scancode);
            if(code < 0 || is_modifier(code)) continue;
            kq_push(ev[i].type == SDL_KEYDOWN ? 3 : 4, code, modifiers());
        }
    }
}

/* The event message for a key (key code << 8 | character), for keys that
 * arrive while macrecomp runs a modal loop. -1 for modifiers alone. */
int pop_mac_key_message(int scancode){
    int code = mac_code((SDL_Scancode)scancode);
    if(code < 0 || is_modifier(code)) return -1;
    return (code << 8) | mac_char(code, (SDL_GetModState() & KMOD_SHIFT) != 0);
}

/* ---- menu commands from the front-end ----
 * A pending (menu, item) goes to the game as a Command-key event whose
 * MenuKey answer is that item: the game handles it exactly as a menu pick. */
#define MAXFORCED 16
static uint32_t g_forced[MAXFORCED]; static int g_nforced;
void pop_inject_menu(int menu, int item){
    if(g_nforced >= MAXFORCED) return;
    g_forced[g_nforced++] = ((uint32_t)menu << 16) | (uint32_t)item;
    kq_push(3, 0x7F, 0x0100 | 0x0080);       /* a Command-key with an unused code */
}
/* 1 and the choice if a forced one is waiting; the keyDown that carries it
 * has key code $7F, character 0. */
uint32_t pop_take_forced_menu(void){
    if(!g_nforced) return 0;
    uint32_t r = g_forced[0];
    memmove(g_forced, g_forced + 1, (size_t)(--g_nforced) * sizeof *g_forced);
    return r;
}

/* GetNextEvent / EventAvail with a key event waiting: deliver it here. The
 * arguments are still on the stack: evt pointer at SP, mask at SP+4. */
int pop_key_event(int peek){
    script_tick();
    pull_sdl_keys();
    if(g_kh == g_kt) return 0;
    uint32_t evp = m68k_r32(SP);
    uint16_t mask = (uint16_t)m68k_r16(SP + 4);
    KeyEv *k = &g_kq[g_kh];
    if(!(mask & (1u << k->what))) return 0;   /* not asked for: stays queued */
    if(evp){
        int h, v; plat_get_mouse(&h, &v);
        m68k_w16(evp, (uint16_t)k->what); m68k_w32(evp + 2, (uint32_t)k->msg);
        m68k_w32(evp + 6, plat_ticks());
        m68k_w16(evp + 10, (uint16_t)v); m68k_w16(evp + 12, (uint16_t)h);
        m68k_w16(evp + 14, (uint16_t)(k->mods | 1));
    }
    if(!peek) g_kh = (g_kh + 1) % KQ;
    SP += 6;
    m68k_w16(SP, 0x0100);                /* Boolean TRUE in the result slot */
    plat_present();
    return 1;
}

/* GetKeys(VAR theKeys: KeyMap): 16 bytes, key code k at byte k/8, bit k%8. */
void pop_get_keys(uint32_t km){
    script_tick();
    pull_sdl_keys();
    uint8_t map[16]; memcpy(map, g_held_script, 16);
    if(pop_ui_menu_open()) memset(map, 0, sizeof map);   /* the menu has the keyboard */
    int n; const Uint8 *st = pop_ui_menu_open() ? NULL : SDL_GetKeyboardState(&n);
    if(!st) n = 0;
    for(int s = 0; s < n; s++){
        if(!st[s]) continue;
        int c = mac_code((SDL_Scancode)s);
        if(c >= 0) map[c >> 3] |= (uint8_t)(1 << (c & 7));
    }
    for(int i = 0; i < 16; i++) m68k_w8(km + (uint32_t)i, map[i]);
}

/* MenuKey(ch): which menu item has this Command-key equivalent. Searched in
 * the game's MENU resources (title Pascal string at 14, then items: name,
 * icon, key, mark, style). */
uint32_t pop_menu_key(int ch, const uint8_t *(*menu)(int id, int *len)){
    ch = toupper(ch & 0xFF);
    if(ch <= ' ') return 0;                  /* no item has "nothing" as its key */
    for(int id = 1000; id <= 1010; id++){
        int len; const uint8_t *b = menu(id, &len);
        if(!b || len < 15) continue;
        int i = 15 + b[14], item = 1;
        while(i < len && b[i]){
            int l = b[i];
            if(i + l + 4 >= len) break;
            if(toupper(b[i + 1 + l + 1]) == ch) return ((uint32_t)id << 16) | (uint32_t)item;
            i += l + 5; item++;
        }
    }
    return 0;
}
