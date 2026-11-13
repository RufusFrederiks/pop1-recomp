/* light.h - torch lighting, a recomp-only effect (see light.c). */
#ifndef POP1_LIGHT_H
#define POP1_LIGHT_H
#include <SDL.h>
#include <stdint.h>

/* Once per presented frame, in a colour mode. on: lighting wanted (setting
 * on, not the Original look, no dialog up). scr: the 8-bit screen (w x h,
 * rowbytes), clut: its colours now. top: 1 when called at the top of the
 * game's frame (the room variables are only read then; mid-frame the game
 * may be drawing a neighbouring room). */
void light_update(int on, const uint8_t *scr, int w, int h, int rowbytes,
                  const uint32_t clut[256], int top);
/* Light the picture in the current render target, which holds the game
 * frame (w x h) at k times its size. Draws nothing when there is nothing to
 * light. Uses SDL render blend modes only (MOD and ADD). */
void light_draw(SDL_Renderer *ren, int k);
/* 1 if light_draw would draw anything. */
int light_visible(void);
void light_free(void);

/* For the enhanced renderer (gfx.c), which draws its own light from the same
 * facts. A source that is lit now: kind 0 a torch, 1 a potion; x, y its
 * centre on the game frame; rgb a potion's colour (0-1). */
typedef struct { int kind, key; float x, y, r, g, b, phase; } LightSrc;
int light_sources(LightSrc *out, int max);
/* How much of the effect to show (0-1: room fades, palette fades). */
float light_level(void);
/* The room on screen: tiles[10..39] its 30 tile types, tiles[0..9] the bottom
 * row of the room above (255 = none). The layout of the tiles on the frame.
 * Returns a number that changes with the room (0 = no room). */
int light_room(unsigned char tiles[40], float *tile_w, float *top, float *row_h);
/* The Prince (0) and the guard (1), if in the room on screen: where the feet
 * are on the frame, how tall the figure stands there (0 = lying dead), and
 * whether it is on the ground. Returns how many (they keep their slot: check
 * `there`). */
typedef struct { int there, grounded, frame; float x, y, h; } LightChar;
void light_chars(LightChar out[2]);

#endif
