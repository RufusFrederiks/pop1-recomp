/* dialog_state.c - macrecomp's dialog.c with its dialog table saved into
 * savestates (normally empty: states are taken in the game's main loop).
 * Included unchanged. */
#include "../../ext/macrecomp/runtime/dialog.c"
#include "state_io.h"

void mrs_dialog(StateIO *io){ ST(io, g_dlg); ST(io, g_param); }
