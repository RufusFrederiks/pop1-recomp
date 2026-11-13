/* gfx.h - the enhanced renderer: shader lighting on the GPU (see gfx.c). */
#ifndef POP1_GFX_H
#define POP1_GFX_H
#include <SDL.h>
#include <stdint.h>

/* Which parts are on (pop1.cfg). */
typedef struct { int bloom, shadows, relief, haze, particles, vignette; } GfxOpts;

/* After the renderer is made. 1 if the enhanced lighting can be used: the
 * renderer is SDL's OpenGL one and the card does shaders, framebuffers and
 * float textures. Says why not in the log. */
int  gfx_init(SDL_Renderer *ren);
int  gfx_available(void);
/* Once per presented frame, before gfx_apply: the game frame (ARGB, w x h)
 * and the settings. Works out the light of the room and the glow. Returns 0
 * when there is nothing to light (not in a room: title, story, fades). */
int  gfx_frame(SDL_Renderer *ren, const uint32_t *frame, int w, int h, const GfxOpts *o);
/* Light the picture in `target`, which must be the current render target
 * and hold the game frame at any whole size. */
void gfx_apply(SDL_Renderer *ren, SDL_Texture *target);
void gfx_free(void);

#endif
