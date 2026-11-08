/* pop1.h - glue between the loader (main.c) and this game's Toolbox layer
 * (traps.c). Everything generic lives in ext/macrecomp; this is only what
 * Prince of Persia needs on top. */
#ifndef POP1_H
#define POP1_H
#include <stdint.h>
#include <stddef.h>
#include "macrecomp/toolbox.h"

/* Load address of CODE segment s (1..4), 0 for anything else. */
uint32_t pop_seg_base(int s);

/* Called once after toolbox_init(), before any game code runs. */
void pop_init(void);

/* Called for every resource the loader registers, so the game layer can
 * notice the ones that are code (MDRV, the music driver). */
void pop_resource_loaded(const char *type, int id, const uint8_t *data, int len);

/* sound.c: the Sound Manager traps PoP uses. 1 if w was one of them. */
int pop_sound_trap(uint16_t w);
/* sound.c: advance the playback clock and retire finished buffers. */
void pop_sound_poll(void);

/* input.c: the keyboard. pop_key_event answers GetNextEvent/EventAvail
 * (arguments still on the stack) when a key event is waiting; 1 if it did. */
int pop_key_event(int peek);
void pop_get_keys(uint32_t keymap);
uint32_t pop_menu_key(int ch, const uint8_t *(*menu)(int id, int *len));

/* qd.c: regions, update clipping, CopyBits. 1 if w was handled. */
int pop_qd_trap(uint16_t w, uint16_t raw);
int pop_update_owed(void);
uint32_t pop_qd_rect_rgn(const Rect *r);
void pop_qd_set_rect_rgn(uint32_t h, int t, int l, int b, int r);

/* cqd.c: Colour QuickDraw, GDevices, the Palette Manager (colour mode only).
 * pop_colour_mode: 0 black-and-white (Mac SE), 1 colour 640x480 (Mac II,
 * Persia(COLOR)), 2 colour 512x384 (Mac LC, Persia(LC)). */
int pop_colour_mode(void);
void pop_cqd_init(int mode);
int pop_cqd_trap(uint16_t w);
int pop_port_is_colour(uint32_t port);
int pop_cqd_fill(const Rect *r, int how);
int pop_cqd_copybits(uint32_t srcB, uint32_t dstB, const Rect *sr, const Rect *dr, int mode,
                     const Rect *mask, int nmask);
uint32_t pop_cqd_new_cwindow(void);
int pop_cqd_set_origin(uint32_t port, int dh, int dv);
void pop_cqd_initgraf(uint32_t globals);
void pop_cqd_sysenvirons(uint32_t rec);
int pop_cqd_shot(const char *path);
int pop_colour_screen(uint32_t *base, int *w, int *h, int *rowbytes);
void pop_colour_clut(uint32_t argb[256]);
void *pop_cqd_state(int *len);
/* platform.c: the graphics mode the player chose (POPMODE, else pop1.cfg). */
int pop_graphics_mode(void);
/* traps.c: the screen area the HAL's dialogs cover now (colour mode). */
int pop_dialog_rect(Rect *out);
/* main.c: start this program again (a new graphics mode). Does not return. */
void pop_restart(void);

/* input.c, continued */
int pop_mac_key_message(int scancode);
void pop_inject_menu(int menu, int item);
void pop_script_poll(void);
uint32_t pop_take_forced_menu(void);
void pop_post_key(int what, int code);

/* pad.c: game controllers (SDL GameController), read into the KeyMap. */
void pop_pad_poll(void);
void pop_pad_keymap(uint8_t map[16]);
int pop_pad_shift(void);
void pop_pad_rumble(int low, int high, int ms);
void pop_pad_sound(int n);
int pop_pad_count(void);
const char *pop_pad_name(void);
int pop_pad_rows(char rows[][2][48], int max);
const char *pop_pad_menu_hint(void);

/* platform.c: the window, the display, the recomp menu. pop_ui_key takes an
 * SDL_Event* (void* here, so this header needs no SDL). */
int pop_ui_key(const void *sdl_event);
int pop_ui_menu_open(void);
void pop_frame(void);
void pop_note_new_game(void);
void pop_tick_present(void);
/* state.c: savestates. what 1 save, 2 load; slot 1..9. Done at the top of
 * the game's loop (pop_state_poll, from pop_frame). */
void pop_state_request(int what, int slot);
void pop_state_poll(void);
int pop_state_info(int slot, char *out, size_t n);
/* platform.c: a short message over the picture; the clock (plat_ticks is the
 * game's clock and can be set, pop_wall_ticks is time since start-up, for
 * the debug knobs). */
void pop_toast(const char *msg);
void pop_set_ticks(uint32_t t);
uint32_t pop_wall_ticks(void);
/* input.c: forget queued and scripted-held keys (after a state load). */
void pop_flush_keys(void);
int pop_take_key(int *what, int *msg);
/* heap.c: NewHandle/NewPtr/Dispose* with reuse of freed blocks. */
int pop_heap_trap(uint16_t w);
void pop_heap_report(void);

/* traps.c / main.c */
const uint8_t *pop_res(const char *type, int id, int *len);
const char *pop_work_dir(void);

/* macrecomp's own trap dispatcher and time slice, renamed at compile time
 * (see CMakeLists.txt) so this game can sit in front of them. */
void mr_toolbox_trap(uint16_t word);
void mr_core_time_slice(void);

#endif
