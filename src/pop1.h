/* pop1.h - glue between the loader (main.c) and this game's Toolbox layer
 * (traps.c). Everything generic lives in ext/macrecomp; this is only what
 * Prince of Persia needs on top. */
#ifndef POP1_H
#define POP1_H
#include <stdint.h>

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

/* input.c, continued */
int pop_mac_key_message(int scancode);
void pop_inject_menu(int menu, int item);
void pop_script_poll(void);
uint32_t pop_take_forced_menu(void);

/* platform.c: the window, the display, the recomp menu. pop_ui_key takes an
 * SDL_Event* (void* here, so this header needs no SDL). */
int pop_ui_key(const void *sdl_event);
int pop_ui_menu_open(void);
void pop_frame(void);
void pop_note_new_game(void);

/* traps.c / main.c */
const uint8_t *pop_res(const char *type, int id, int *len);
const char *pop_work_dir(void);

/* macrecomp's own trap dispatcher and time slice, renamed at compile time
 * (see CMakeLists.txt) so this game can sit in front of them. */
void mr_toolbox_trap(uint16_t word);
void mr_core_time_slice(void);

#endif
