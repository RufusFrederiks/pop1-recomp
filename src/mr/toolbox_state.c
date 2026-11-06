/* toolbox_state.c - macrecomp's toolbox.c, compiled here so that its
 * file-local state (window list, heap pointer, resource handles, handle sizes)
 * can be saved and restored. The file is included unchanged: the submodule is
 * not modified. Built with -Dm68k_trap=mr_toolbox_trap like toolbox.c itself
 * (see CMakeLists.txt). */
#include "../../ext/macrecomp/runtime/toolbox.c"
#include "state_io.h"

void mrs_toolbox(StateIO *io){
    ST(io, g_front_win); ST(io, g_update_pending);
    ST(io, g_peek_valid); ST(io, g_peek_what); ST(io, g_peek_msg); ST(io, g_peek_h); ST(io, g_peek_v);
    ST(io, g_cur_port);
    ST(io, g_wins); ST(io, g_nwins); ST(io, g_win_pending); ST(io, g_win_activate); ST(io, g_win_bounds);
    ST(io, heap_ptr); ST(io, heap_end);
    ST(io, g_curres); ST(io, g_front_dlg); ST(io, g_rand); ST(io, g_fmout); ST(io, g_widths);
    /* Resource handles: which resources have been loaded, and where. The data
     * pointers are this process's (the fork buffer) and stay as they are. */
    int n = g_nres; ST(io, n);
    if(!io->save && n != g_nres){ io->err = 1; return; }
    for(int i = 0; i < g_nres; i++) ST(io, g_res[i].handle);
    /* Handle sizes: the whole open-addressed table. */
    uint32_t cap = g_hsz_cap, cnt = g_hsz_n; ST(io, cap); ST(io, cnt);
    if(!io->save){
        HSz *t = (HSz*)realloc(g_hsz, (size_t)(cap ? cap : 1) * sizeof *t);
        if(!t){ io->err = 1; return; }
        g_hsz = t; g_hsz_cap = cap; g_hsz_n = cnt;
    }
    if(cap) st_bytes(io, g_hsz, (size_t)cap * sizeof *g_hsz);
}

/* After a savestate load: states made before the dialog-port fix can hold a
 * closed dialog as the current port. At the top of the game's loop the
 * current port is always a window; if it is not, make it the front window. */
void mrs_repair_port(void){
    /* a window made by NewWindow has windowKind userKind (8); a dialog here
     * has none */
    if(!g_front_win || g_cur_port == g_front_win) return;
    if(g_cur_port && m68k_r16(g_cur_port + 108) == 8) return;
    fprintf(stderr, "[state] current port %06x is not a window: now %06x\n", g_cur_port, g_front_win);
    SP -= 4; m68k_w32(SP, g_front_win);
    m68k_trap(0xA873);                       /* SetPort (this file's dispatcher) */
}

/* For src/heap.c: the handle-size table, and whether a handle is one the
 * Resource Manager hands out (and may hand out again). */
uint32_t mrs_hsz_get(uint32_t h){ return hsz_get(h); }
int mrs_hsz_known(uint32_t h){ return hsz_known(h); }
void mrs_hsz_set(uint32_t h, uint32_t sz){ hsz_set(h, sz); }
int mrs_is_res_handle(uint32_t h){
    for(int i = 0; i < g_nres; i++) if(g_res[i].handle == h) return 1;
    return 0;
}
