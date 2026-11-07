/* filters.h - pixel-art scalers for the Remastered looks (see filters.c). */
#ifndef POP1_FILTERS_H
#define POP1_FILTERS_H
#include <stdint.h>

/* src: w x h ARGB. dst: kw x kh ARGB, k = 3 or 4. Only source rows
 * [y0, y1) are done (output rows [k*y0, k*y1)); the filters read up to 2
 * rows beyond. */
void filter_advmame(const uint32_t *src, int w, int h, uint32_t *dst, int k, int y0, int y1);
void filter_xbr(const uint32_t *src, int w, int h, uint32_t *dst, int k, int y0, int y1);
#define FILTER_REACH 2

#endif
