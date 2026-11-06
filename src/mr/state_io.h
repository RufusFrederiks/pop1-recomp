/* state_io.h - the savestate stream every module writes its host-side state
 * into (src/state.c owns the file). st_bytes saves or loads n bytes in place,
 * framed with its length so a layout change is caught instead of misread. */
#ifndef POP_STATE_IO_H
#define POP_STATE_IO_H
#include <stdio.h>
#include <stddef.h>
typedef struct { FILE *f; int save; int err; } StateIO;
void st_bytes(StateIO *io, void *p, size_t n);
#define ST(io, v) st_bytes((io), &(v), sizeof(v))

/* The pieces (each in the file that can see the state). */
void mrs_toolbox(StateIO *io);     /* src/mr/toolbox_state.c */
void mrs_repair_port(void);        /* src/mr/toolbox_state.c */
void mrs_quickdraw(StateIO *io);   /* src/mr/quickdraw_state.c */
void mrs_files(StateIO *io);       /* src/mr/files_state.c */
void mrs_dialog(StateIO *io);      /* src/mr/dialog_state.c */
void pop_traps_state(StateIO *io); /* src/traps.c */
void pop_qd_state(StateIO *io);    /* src/qd.c */
void pop_sound_state(StateIO *io); /* src/sound.c */
void pop_heap_state(StateIO *io);  /* src/heap.c */
void pop_heap_reset(void);
#endif
