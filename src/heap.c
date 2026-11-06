/* heap.c - freed memory is used again.
 *
 * macrecomp's Memory Manager is a bump allocator: NewHandle and NewPtr take
 * fresh memory and DisposeHandle/DisposePtr do nothing. Prince of Persia
 * saves the background behind each moving sprite in a new handle every frame
 * and disposes of it the next (CODE 2 0x3f7a / 0x3fdc, called through the
 * jump table for every sprite), so the heap grew by about 2 MB a minute and a
 * long game would run it out (8-30 MB). This takes NewHandle, NewPtr and the
 * two Dispose calls first:
 *
 *  - every block handed out is remembered with its capacity;
 *  - a disposed block goes into a short quarantine first (a game that touches
 *    memory just after freeing it, which a real Mac usually forgives, still
 *    finds its data), then into a free list kept in address order, merged
 *    with its neighbours;
 *  - a new request takes the smallest free block that fits (splitting off
 *    the rest), and only when none does goes to the bump allocator.
 *
 * Handles the Resource Manager hands out (and would hand out again) are never
 * freed here, unless the game detached them. Blocks come back zeroed, as the
 * bump allocator's do. The tables are saved in savestates. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include "mr/state_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t mrs_hsz_get(uint32_t h);
int mrs_hsz_known(uint32_t h);
void mrs_hsz_set(uint32_t h, uint32_t sz);
int mrs_is_res_handle(uint32_t h);
int pop_is_detached(uint32_t h);

typedef struct { uint32_t addr, cap; } Blk;
#define NLIVE 8192
#define NFREE 4096
#define NQUAR 64
#define NMAST 4096
static struct {
    Blk live[NLIVE]; int nlive;            /* blocks handed out: address, capacity */
    Blk freel[NFREE]; int nfree;           /* free, sorted by address, merged */
    Blk quar[NQUAR]; int qhead, nq;        /* freed recently (FIFO) */
    uint32_t mast[NMAST]; int nmast;       /* free master pointers */
    uint32_t reused, bumped;               /* statistics */
} H;

static uint32_t round_cap(uint32_t n){ if(!n) n = 16; return (n + 3) & ~3u; }

static void live_add(uint32_t a, uint32_t cap){
    if(H.nlive < NLIVE){ H.live[H.nlive].addr = a; H.live[H.nlive].cap = cap; H.nlive++; }
}
static uint32_t live_take(uint32_t a){        /* capacity, 0 if unknown */
    for(int i = H.nlive - 1; i >= 0; i--) if(H.live[i].addr == a){
        uint32_t c = H.live[i].cap; H.live[i] = H.live[--H.nlive]; return c; }
    return 0;
}

static void free_insert(uint32_t a, uint32_t cap){
    /* find the place (address order) */
    int lo = 0, hi = H.nfree;
    while(lo < hi){ int m = (lo + hi) / 2; if(H.freel[m].addr < a) lo = m + 1; else hi = m; }
    /* overlaps a free block: a double free, ignore it */
    if(lo > 0 && H.freel[lo-1].addr + H.freel[lo-1].cap > a) return;
    if(lo < H.nfree && a + cap > H.freel[lo].addr) return;
    int merged_prev = lo > 0 && H.freel[lo-1].addr + H.freel[lo-1].cap == a;
    int merged_next = lo < H.nfree && a + cap == H.freel[lo].addr;
    if(merged_prev && merged_next){
        H.freel[lo-1].cap += cap + H.freel[lo].cap;
        memmove(&H.freel[lo], &H.freel[lo+1], sizeof(Blk) * (size_t)(H.nfree - lo - 1)); H.nfree--;
    } else if(merged_prev) H.freel[lo-1].cap += cap;
    else if(merged_next){ H.freel[lo].addr = a; H.freel[lo].cap += cap; }
    else {
        if(H.nfree >= NFREE) return;             /* full: let this one go */
        memmove(&H.freel[lo+1], &H.freel[lo], sizeof(Blk) * (size_t)(H.nfree - lo));
        H.freel[lo].addr = a; H.freel[lo].cap = cap; H.nfree++;
    }
}
static void release(uint32_t a, uint32_t cap){
    if(H.nq == NQUAR){                           /* the oldest leaves quarantine */
        Blk o = H.quar[H.qhead]; H.qhead = (H.qhead + 1) % NQUAR; H.nq--;
        free_insert(o.addr, o.cap);
    }
    H.quar[(H.qhead + H.nq) % NQUAR].addr = a; H.quar[(H.qhead + H.nq) % NQUAR].cap = cap; H.nq++;
}
/* the smallest free block of at least cap bytes: its address, or 0 */
static uint32_t take(uint32_t cap){
    int best = -1;
    for(int i = 0; i < H.nfree; i++)
        if(H.freel[i].cap >= cap && (best < 0 || H.freel[i].cap < H.freel[best].cap)){
            best = i; if(H.freel[i].cap == cap) break; }
    if(best < 0) return 0;
    uint32_t a = H.freel[best].addr;
    if(H.freel[best].cap - cap >= 32){ H.freel[best].addr += cap; H.freel[best].cap -= cap; }
    else { cap = H.freel[best].cap;
           memmove(&H.freel[best], &H.freel[best+1], sizeof(Blk) * (size_t)(H.nfree - best - 1)); H.nfree--; }
    memset(M.mem + a, 0, cap);
    live_add(a, cap);
    H.reused++;
    return a;
}

int pop_heap_trap(uint16_t w){
    switch(w){
    case 0xA122: { /* NewHandle(D0 = size): A0 = handle, D0 = result */
        uint32_t n = M.d[0], cap = round_cap(n), blk = take(cap);
        if(!blk){                                   /* the bump allocator, remembered */
            mr_toolbox_trap(0xA122);
            if(M.a[0] && m68k_r32(M.a[0])){ live_add(m68k_r32(M.a[0]), cap); H.bumped++; }
            return 1;
        }
        uint32_t h = H.nmast ? H.mast[--H.nmast] : mr_alloc(4);
        if(!h){ live_take(blk); free_insert(blk, cap); M.a[0] = 0; M.d[0] = (uint32_t)-108; fl_logic(M.d[0], 2); return 1; }
        m68k_w32(h, blk); mrs_hsz_set(h, n);
        M.a[0] = h; M.d[0] = 0; fl_logic(0, 2);
        return 1; }
    case 0xA11E: { /* NewPtr(D0 = size): A0 = pointer */
        uint32_t n = M.d[0], cap = round_cap(n), blk = take(cap);
        if(!blk){
            mr_toolbox_trap(0xA11E);
            if(M.a[0]){ live_add(M.a[0], cap); H.bumped++; }
            return 1;
        }
        M.a[0] = blk; M.d[0] = 0; fl_logic(0, 2);
        return 1; }
    case 0xA023: { /* DisposeHandle(A0) */
        uint32_t h = M.a[0];
        if(h && h < M.memsize - 4 && mrs_hsz_known(h) && (!mrs_is_res_handle(h) || pop_is_detached(h))){
            uint32_t blk = m68k_r32(h), cap = blk ? live_take(blk) : 0;
            if(cap){
                release(blk, cap);
                m68k_w32(h, 0); mrs_hsz_set(h, 0);
                if(!mrs_is_res_handle(h) && H.nmast < NMAST) H.mast[H.nmast++] = h;
            }
        }
        M.d[0] = 0; fl_logic(0, 2);
        return 1; }
    case 0xA01F: { /* DisposePtr(A0) */
        uint32_t p = M.a[0], cap = p ? live_take(p) : 0;
        if(cap) release(p, cap);
        M.d[0] = 0; fl_logic(0, 2);
        return 1; }
    }
    return 0;
}

void pop_heap_state(StateIO *io){ ST(io, H); }
/* A state from before this file existed: its blocks are unknown here, so
 * nothing of it is ever reused (the tables of this run describe memory that
 * the state has just replaced). */
void pop_heap_reset(void){ memset(&H, 0, sizeof H); }

/* POPHEAPSTAT: how much was reused (printed at exit). */
void pop_heap_report(void){
    fprintf(stderr, "[heap] %u blocks reused, %u from the bump heap, %d live, %d free blocks\n",
            H.reused, H.bumped, H.nlive, H.nfree);
}
