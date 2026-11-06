/* files_state.c - macrecomp's files.c with its open files (fork, position)
 * saved into savestates. Included unchanged. The registered files themselves
 * are this process's (paths on disk) and are not saved. */
#include "../../ext/macrecomp/runtime/files.c"
#include "state_io.h"

void mrs_files(StateIO *io){ ST(io, g_open); }
