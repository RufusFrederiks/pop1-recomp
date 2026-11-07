/* crt.h - the CRT looks (see crt.c). */
#ifndef POP1_CRT_H
#define POP1_CRT_H
#include <SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* Draw the game as a CRT into the rectangle d of the window (the current
 * render target must be the window). kind: 0 = Monitor, 1 = TV.
 * game: the 1x frame (W x H), view: the part shown, stage: the same frame
 * at k times its size (nearest neighbour), or NULL. 0 = could not (the
 * caller then draws the plain picture). */
int crt_draw(SDL_Renderer *ren, int kind, SDL_Texture *game, const SDL_Rect *view,
             SDL_Texture *stage, int k, const SDL_Rect *d);
void crt_free(void);

#endif
