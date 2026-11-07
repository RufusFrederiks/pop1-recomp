/* filters.h - pixel-art scalers for the Remastered looks (see filters.c). */
#ifndef POP1_FILTERS_H
#define POP1_FILTERS_H
#include <stdint.h>

/* src: w x h ARGB. dst: 4w x 4h ARGB. Only source rows [y0, y1) are done
 * (output rows [4*y0, 4*y1)); the filters read up to 2 rows beyond. */
void filter_advmame4x(const uint32_t *src, int w, int h, uint32_t *dst, int y0, int y1);
void filter_xbr4x(const uint32_t *src, int w, int h, uint32_t *dst, int y0, int y1);
#define FILTER_REACH 2

#endif
