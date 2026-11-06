/* sound.c - the part of the Sound Manager Prince of Persia uses, on SDL2 audio.
 *
 * PoP's music driver (MDRV, James Nitchals 1991-92) does its own mixing. It
 * opens one sampledSynth channel and feeds it 8-bit buffers with bufferCmd;
 * flushCmd and quietCmd stop it. Two things about how it does that decide the
 * design here:
 *
 *  - The buffer is a ring. The driver queues the same 2-second buffer twice
 *    and keeps mixing into it just ahead of where it reckons playback is. The
 *    Sound Manager it was written for played straight out of that memory, so
 *    the samples must be read from guest memory when they are played, not
 *    copied when the command arrives.
 *  - It paces itself on the queue. It mixes the next stretch when it sees the
 *    channel's qHead move (it compares the word at chan+32 every tick), which
 *    happens when the Sound Manager finishes a queued command.
 *
 * So: queued bufferCmds wait in a play list; pop_sound_poll(), called at every
 * trap and loop time slice, works out how many output samples the wall clock
 * says have played since last time, reads them out of guest memory, pushes
 * them to SDL, and advances qHead as each buffer finishes. With no audio
 * device the clock still runs, so the game's timing does not depend on one.
 *
 * POPWAV=<file.wav> also writes the output to a WAV file, which is how audio
 * can be checked on a machine without speakers. POPSOUND=1 traces commands.
 *
 * Semantics from Inside Macintosh: Sound (1994). */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include "pop1.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 22050
#define RING (OUT_RATE * 2)

/* ---- output: an SDL ring the main thread fills and the callback drains ---- */
static SDL_AudioDeviceID g_dev;
static int16_t g_ring[RING];
static volatile unsigned g_head, g_tail;
static FILE *g_wav;
static uint32_t g_wav_n;
static int g_trace = -1;

static void SDLCALL audio_cb(void *u, Uint8 *out, int len){
    (void)u;
    int16_t *o = (int16_t*)out; int n = len / 2;
    for(int i = 0; i < n; i++){
        if(g_tail != g_head){ o[i] = g_ring[g_tail]; g_tail = (g_tail + 1) % RING; }
        else o[i] = 0;
    }
}

static void wav_header(FILE *f, uint32_t nsamples){
    uint32_t data = nsamples * 2, riff = 36 + data, rate = OUT_RATE, br = OUT_RATE * 2;
    uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,
                     0,0,0,0,0,0,0,0,2,0,16,0,'d','a','t','a',0,0,0,0};
    memcpy(h+4, &riff, 4); memcpy(h+24, &rate, 4); memcpy(h+28, &br, 4); memcpy(h+40, &data, 4);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, 0, SEEK_END);
}
static void wav_close(void){ if(g_wav){ wav_header(g_wav, g_wav_n); fclose(g_wav); g_wav = NULL; } }

static void out_sample(int16_t s){
    if(g_dev){
        unsigned nh = (g_head + 1) % RING;
        if(nh != g_tail){ g_ring[g_head] = s; g_head = nh; }
    }
    if(g_wav){ fwrite(&s, 2, 1, g_wav); g_wav_n++; }
}

static int sound_open(void){
    static int tried;
    if(tried) return 1;
    tried = 1;
    if(g_trace < 0) g_trace = getenv("POPSOUND") != 0;
    const char *w = getenv("POPWAV");
    if(w && (g_wav = fopen(w, "wb"))){ wav_header(g_wav, 0); atexit(wav_close); }
    if(getenv("POPNOAUDIO")) return 1;
    if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0){
        fprintf(stderr, "[sound] no audio (%s); running silent\n", SDL_GetError());
        return 1;
    }
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 1;
    want.samples = 512; want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if(!g_dev){ fprintf(stderr, "[sound] no audio device (%s); running silent\n", SDL_GetError()); return 1; }
    SDL_PauseAudioDevice(g_dev, 0);
    return 1;                            /* silent is still a working channel */
}

/* ---- channels and the play list ---- */

/* SndChannel: nextChan(0) firstMod(4) callBack(8) userInfo(12) wait(16)
 * cmdInProgress(20, 8 bytes) flags(28) qLength(30) qHead(32) qTail(34)
 * queue(36, 8 bytes per command). */
#define CH_QLEN 30
#define CH_QHEAD 32
#define CH_QTAIL 34
#define CH_QUEUE 36
#define QLEN 128

typedef struct {
    uint32_t chan;
    uint32_t ptr, len;        /* 8-bit offset-binary samples in guest memory */
    double step;              /* input samples per output sample */
    double pos;
} Play;
#define MAX_PLAY 64
static Play g_play[MAX_PLAY];
static int g_nplay;
static uint32_t g_clock_ms;
static double g_owed;          /* output samples due but not yet produced */

static void q_advance(uint32_t chan){
    uint16_t h = (uint16_t)m68k_r16(chan + CH_QHEAD);
    if(h == (uint16_t)m68k_r16(chan + CH_QTAIL)) return;
    m68k_w16(chan + CH_QHEAD, (uint16_t)((h + 1) % QLEN));
}
static void q_push(uint32_t chan, uint32_t cmd){
    uint16_t t = (uint16_t)m68k_r16(chan + CH_QTAIL);
    for(int i = 0; i < 8; i++) m68k_w8(chan + CH_QUEUE + t*8u + i, m68k_r8(cmd + i));
    m68k_w16(chan + CH_QTAIL, (uint16_t)((t + 1) % QLEN));
}
static void q_flush(uint32_t chan){
    m68k_w16(chan + CH_QHEAD, (uint16_t)m68k_r16(chan + CH_QTAIL));
    int k = 0;
    for(int i = 0; i < g_nplay; i++) if(g_play[i].chan != chan) g_play[k++] = g_play[i];
    g_nplay = k;
    if(!g_nplay) g_owed = 0;
}

void pop_sound_poll(void){
    uint32_t now = SDL_GetTicks();
    if(!g_clock_ms){ g_clock_ms = now; return; }
    uint32_t dt = now - g_clock_ms;
    if(!dt) return;
    g_clock_ms = now;
    if(!g_nplay){ g_owed = 0; return; }   /* idle: nothing is owed for silence */
    if(dt > 250) dt = 250;                /* a stall: do not dump seconds at once */
    g_owed += (double)dt * OUT_RATE / 1000.0;
    while(g_owed >= 1.0 && g_nplay){
        Play *p = &g_play[0];
        uint32_t i = (uint32_t)p->pos; double f = p->pos - i;
        int a = (int)m68k_r8(p->ptr + i) - 128;
        int b = (i + 1 < p->len) ? (int)m68k_r8(p->ptr + i + 1) - 128 : a;
        out_sample((int16_t)((a + (b - a) * f) * 192));   /* 8 -> 16 bit, some headroom */
        g_owed -= 1.0;
        p->pos += p->step;
        if(p->pos >= (double)p->len){    /* finished: the Sound Manager moves on */
            uint32_t chan = p->chan;
            memmove(g_play, g_play + 1, (size_t)(--g_nplay) * sizeof *g_play);
            if(chan) q_advance(chan);
        }
    }
}

/* A sampled-sound header (standard, encode 0): samplePtr(4) length(4)
 * sampleRate(Fixed 4) loopStart(4) loopEnd(4) encode(1) baseFrequency(1)
 * sampleArea[]. */
static int queue_buffer(uint32_t chan, uint32_t hdr){
    if(!hdr) return 0;
    uint32_t ptr = m68k_r32(hdr), len = m68k_r32(hdr + 4), rate = m68k_r32(hdr + 8);
    int enc = (int)m68k_r8(hdr + 20);
    if(!ptr) ptr = hdr + 22;
    if(enc != 0){ static int said; if(!said++) fprintf(stderr, "[sound] encode %d not supported\n", enc); return 0; }
    if(!rate || !len || len > 0x400000 || g_nplay >= MAX_PLAY) return 0;
    if(g_trace > 0){ static int n; if(n++ < 40) fprintf(stderr,
        "[sound] buffer %u samples at %.1f Hz, t=%u ms\n", len, rate / 65536.0, SDL_GetTicks()); }
    if(!g_nplay) g_clock_ms = SDL_GetTicks();     /* starting from silence */
    Play *p = &g_play[g_nplay++];
    p->chan = chan; p->ptr = ptr; p->len = len; p->pos = 0;
    p->step = (double)rate / 65536.0 / OUT_RATE;
    return 1;
}

static void do_command(uint32_t chan, uint32_t cmd, int queued){
    uint16_t c = (uint16_t)m68k_r16(cmd);
    uint32_t p2 = m68k_r32(cmd + 4);
    if(g_trace < 0) g_trace = getenv("POPSOUND") != 0;
    if(g_trace > 0){ static int n; if(n++ < 200) fprintf(stderr, "[sound] chan %06x cmd %u p1 %d p2 %06x%s\n",
                                                      chan, c, (int16_t)m68k_r16(cmd + 2), p2, queued ? "" : " (now)"); }
    if(queued && chan) q_push(chan, cmd);
    switch(c & 0x7FFF){                  /* high bit: dataPointerFlag */
    case 81: /* bufferCmd: completes when it has played */
        if(queue_buffer(queued ? chan : 0, p2)) return;
        break;
    case 3:  /* quietCmd */
    case 4:  /* flushCmd */
        if(chan) q_flush(chan);
        if(g_dev){ SDL_LockAudioDevice(g_dev); g_tail = g_head; SDL_UnlockAudioDevice(g_dev); }
        return;
    default: break;                      /* nullCmd, ampCmd, ...: nothing the driver sends */
    }
    if(queued && chan) q_advance(chan);  /* anything else completes at once */
}

/* Pascal argument popping, as toolbox.c does it. */
static uint32_t pop32(void){ uint32_t v = m68k_r32(SP); SP += 4; return v; }
static uint16_t pop16(void){ uint16_t v = (uint16_t)m68k_r16(SP); SP += 2; return v; }

int pop_sound_trap(uint16_t w){
    switch(w){
    case 0xA807: { /* SndNewChannel(VAR chan, synth, init, userRoutine): OSErr */
        uint32_t user = pop32(); uint32_t init = pop32(); uint16_t synth = pop16();
        uint32_t pchan = pop32();
        sound_open();
        uint32_t chan = pchan ? m68k_r32(pchan) : 0;
        if(!chan){ chan = mr_alloc(CH_QUEUE + QLEN*8); if(pchan) m68k_w32(pchan, chan); }
        if(chan){
            m68k_w32(chan + 8, user);      /* callBack */
            m68k_w16(chan + CH_QLEN, QLEN);
            m68k_w16(chan + CH_QHEAD, 0); m68k_w16(chan + CH_QTAIL, 0);
        }
        if(g_trace > 0) fprintf(stderr, "[sound] SndNewChannel synth %u init %x -> %06x\n", synth, init, chan);
        m68k_w16(SP, chan ? 0 : (uint16_t)-108);
        return 1; }
    case 0xA803: { /* SndDoCommand(chan, VAR cmd, noWait): OSErr */
        (void)pop16(); uint32_t cmd = pop32(); uint32_t chan = pop32();
        do_command(chan, cmd, 1); m68k_w16(SP, 0); return 1; }
    case 0xA804: { /* SndDoImmediate(chan, VAR cmd): OSErr */
        uint32_t cmd = pop32(); uint32_t chan = pop32();
        do_command(chan, cmd, 0); m68k_w16(SP, 0); return 1; }
    case 0xA801: { /* SndDisposeChannel(chan, quietNow): OSErr */
        (void)pop16(); uint32_t chan = pop32();
        if(chan) q_flush(chan);
        m68k_w16(SP, 0); return 1; }
    case 0xA006: /* KillIO(D0 = refNum): the old Sound Driver, nothing to stop */
        M.d[0] = 0; fl_logic(0, 2); return 1;
    }
    return 0;
}
