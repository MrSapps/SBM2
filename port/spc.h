/* The sound side: an SPC700 with its timers, the S-DSP (32 kHz stereo) and
 * a high-level boot ROM (IPL) that speaks the boot ROM's upload protocol.
 *
 * The port's machine (snes.c) keeps its fake APU: the game's replays were
 * recorded against it and must not change. For sound, build/port/play runs
 * an Spc beside it as a shadow, fed the CPU's port writes in order. Where
 * the CPU saw an APU port change (a handshake it waited for), the shadow
 * holds the later writes back until its own port shows that value
 * (spc_feed_*), so the real sound driver gets every byte and command at
 * its own pace. */
#ifndef SBM2_SPC_H
#define SBM2_SPC_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int16_t buf[24];            /* decoded samples, 12 twice (no wrap in the window) */
    int buf_pos;                /* where the next 4 decoded samples go (0, 4, 8) */
    int interp_pos;             /* 4.12 fixed: sample position */
    int brr_addr, brr_offset;
    int kon_delay;
    int env_mode, env, hidden_env;
    int out;                    /* last output (after the envelope), for PMON */
} SpcVoice;

typedef struct {
    uint8_t regs[128];
    SpcVoice v[8];
    int every_other, counter;
    int new_kon, kon, koff;
    int noise;
    int echo_offset, echo_length;
    int16_t echo_hist[16][2];   /* 8 entries, twice */
    int echo_pos;
} SpcDsp;

typedef struct { uint64_t t; uint8_t sync, port, val; } SpcEvent;

typedef struct {
    uint8_t ram[0x10000];
    uint8_t a, x, y, sp, psw;
    uint16_t pc;
    int halted;
    /* $F0-$FF */
    uint8_t control, dspaddr;
    uint8_t in[4];              /* CPU -> SPC ($2140-$2143 written) */
    uint8_t out[4];             /* SPC -> CPU */
    uint8_t target[3], tcount[3], tout[3];
    int tdiv[3];
    /* the boot ROM, high level: state, block index, destination */
    int ipl, ipl_y;
    uint16_t ipl_dest;
    uint64_t cycles;
    int sample_div;             /* cycles toward the next DSP sample (32) */
    SpcDsp dsp;
    /* output: stereo samples since the caller last took them */
    int16_t *samples;
    size_t nsamples, cap;
    /* the feed */
    SpcEvent *q;
    size_t qhead, qlen, qcap;
    uint64_t wait_since, delay, last_apply;
    long syncs, timeouts;
    uint64_t max_lag;
    int debug;                  /* log the first handshakes that time out */
} Spc;

void spc_init(Spc *s);
/* One instruction (or boot ROM step); returns its cycles. */
int spc_step(Spc *s);
/* Run until s->cycles >= t, applying feed events whose time has come. */
void spc_run_until(Spc *s, uint64_t t);
/* Queue a CPU write to port p, or a handshake: at that point the CPU had
 * seen port p read v. t is in SPC cycles. */
void spc_feed_write(Spc *s, uint64_t t, int p, uint8_t v);
void spc_feed_sync(Spc *s, uint64_t t, int p, uint8_t v);
/* Run until the feed is empty (or max cycles more); returns events left. */
size_t spc_drain(Spc *s, uint64_t max);

uint8_t spc_dsp_read(Spc *s, int a);
void spc_dsp_write(Spc *s, int a, uint8_t v);
/* One DSP sample (appended to s->samples). */
void spc_dsp_sample(Spc *s);

/* The .spc file format (v0.30): a snapshot another player can load. */
size_t spc_save(const Spc *s, uint8_t *out);     /* out: 0x10200 bytes */
int spc_load(Spc *s, const uint8_t *data, size_t n);

#endif
