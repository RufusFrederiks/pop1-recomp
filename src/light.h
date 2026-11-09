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

#endif
