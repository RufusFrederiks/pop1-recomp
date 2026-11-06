/* quickdraw_state.c - macrecomp's quickdraw.c with its pen, clip and drawing
 * target saved into savestates. Included unchanged. */
#include "../../ext/macrecomp/runtime/quickdraw.c"
#include "state_io.h"

void mrs_quickdraw(StateIO *io){
    ST(io, pen_h); ST(io, pen_v); ST(io, pen_w); ST(io, pen_h_sz);
    ST(io, pen_black); ST(io, pen_mode); ST(io, txt_mode); ST(io, clip); ST(io, cur);
}
