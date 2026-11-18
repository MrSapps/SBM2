/* S-DSP: 8 voices of BRR samples, ADSR/GAIN envelopes, noise, pitch
 * modulation, echo with an 8-tap FIR; one stereo sample per 32 SPC700
 * cycles. Sample-level: each voice's work for a sample happens at once
 * (the chip spreads it over the 32 cycles, which only matters to code
 * reading ENVX/OUTX mid-sample). */
#include <stdlib.h>
#include "spc.h"
#include "gauss.h"      /* generated: static const int16_t gauss[512] */

enum { ENV_RELEASE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN };
enum { MVOLL = 0x0C, MVOLR = 0x1C, EVOLL = 0x2C, EVOLR = 0x3C, KON = 0x4C, KOFF = 0x5C, FLG = 0x6C, ENDX = 0x7C,
       EFB = 0x0D, PMON = 0x2D, NON = 0x3D, EON = 0x4D, DIR = 0x5D, ESA = 0x6D, EDL = 0x7D };

#define COUNTER_RANGE (2048 * 5 * 3)
static const int counter_rates[32] = {
    COUNTER_RANGE + 1, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
static const int counter_offsets[32] = {
    1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0};

static int fires(const SpcDsp *d, int rate)
{
    return ((unsigned)d->counter + (unsigned)counter_offsets[rate]) % (unsigned)counter_rates[rate] == 0;
}

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

uint8_t spc_dsp_read(Spc *s, int a) { return s->dsp.regs[a & 0x7F]; }

void spc_dsp_write(Spc *s, int a, uint8_t v)
{
    SpcDsp *d = &s->dsp;
    d->regs[a] = v;
    if (a == KON) d->new_kon = v;
    else if (a == ENDX) d->regs[ENDX] = 0;
}

static void decode_brr(Spc *s, SpcVoice *v)
{
    uint8_t header = s->ram[(uint16_t)v->brr_addr];
    int nyb = s->ram[(uint16_t)(v->brr_addr + v->brr_offset)] << 8 | s->ram[(uint16_t)(v->brr_addr + v->brr_offset + 1)];
    int shift = header >> 4, filter = header & 0x0C;
    int16_t *pos = &v->buf[v->buf_pos];
    for (int i = 0; i < 4; i++, pos++, nyb <<= 4) {
        int smp = (int16_t)nyb >> 12;
        smp = (smp << shift) >> 1;
        if (shift >= 0xD) smp = (smp >> 25) << 11;
        /* the two before (the window holds each sample twice, 12 apart) */
        int p1 = pos[v->buf_pos + i >= 1 ? -1 : 11];
        int p2 = pos[v->buf_pos + i >= 2 ? -2 : 10] >> 1;
        if (filter >= 8) {
            smp += p1;
            smp -= p2;
            if (filter == 8) smp += p2 >> 4, smp += (p1 * -3) >> 6;
            else smp += (p1 * -13) >> 7, smp += (p2 * 3) >> 4;
        } else if (filter) {
            smp += p1 >> 1;
            smp += (-p1) >> 5;
        }
        smp = (int16_t)(clamp16(smp) * 2);
        pos[0] = (int16_t)smp;
        pos[12] = (int16_t)smp;
    }
    v->buf_pos = (v->buf_pos + 4) % 12;
}

/* The envelope, every sample (it moves when its rate's counter fires). */
static void envelope(SpcDsp *d, SpcVoice *v, const uint8_t *vr)
{
    int env = v->env;
    if (v->env_mode == ENV_RELEASE) {
        if ((env -= 8) < 0) env = 0;
        v->env = env;
        return;
    }
    int rate, data = vr[6];
    if (vr[5] & 0x80) {
        if (v->env_mode >= ENV_DECAY) {
            env--;
            env -= env >> 8;
            rate = data & 0x1F;
            if (v->env_mode == ENV_DECAY) rate = (vr[5] >> 3 & 0x0E) + 0x10;
        } else {
            rate = (vr[5] & 0x0F) * 2 + 1;
            env += rate < 31 ? 0x20 : 0x400;
        }
    } else {
        data = vr[7];
        int mode = data >> 5;
        if (mode < 4) {
            env = data * 0x10;
            rate = 31;
        } else {
            rate = data & 0x1F;
            if (mode == 4) env -= 0x20;
            else if (mode < 6) { env--; env -= env >> 8; }
            else {
                env += 0x20;
                if (mode > 6 && (unsigned)v->hidden_env >= 0x600) env += 0x8 - 0x20;
            }
        }
    }
    if ((env >> 8) == (data >> 5) && v->env_mode == ENV_DECAY) v->env_mode = ENV_SUSTAIN;
    v->hidden_env = env;
    if ((unsigned)env > 0x7FF) {
        env = env < 0 ? 0 : 0x7FF;
        if (v->env_mode == ENV_ATTACK) v->env_mode = ENV_DECAY;
    }
    if (fires(d, rate)) v->env = env;
}

static int interpolate(const SpcVoice *v)
{
    int off = v->interp_pos >> 4 & 0xFF;
    const int16_t *fwd = gauss + 255 - off, *rev = gauss + off;
    const int16_t *in = &v->buf[(v->interp_pos >> 12) + v->buf_pos];
    int out = (fwd[0] * in[0]) >> 11;
    out += (fwd[256] * in[1]) >> 11;
    out += (rev[256] * in[2]) >> 11;
    out = (int16_t)out;
    out += (rev[0] * in[3]) >> 11;
    return clamp16(out) & ~1;
}

void spc_dsp_sample(Spc *s)
{
    SpcDsp *d = &s->dsp;
    uint8_t *r = d->regs;
    if (--d->counter < 0) d->counter = COUNTER_RANGE - 1;
    d->every_other ^= 1;
    if (d->every_other) {
        d->kon = d->new_kon;
        d->new_kon = 0;
        d->koff = r[KOFF];
    }
    if (fires(d, r[FLG] & 0x1F)) {
        int fb = d->noise << 13 ^ d->noise << 14;
        d->noise = (fb & 0x4000) ^ (d->noise >> 1);
    }
    int main_l = 0, main_r = 0, echo_l = 0, echo_r = 0, prev_out = 0;
    int dir = r[DIR] << 8;
    for (int i = 0; i < 8; i++) {
        SpcVoice *v = &d->v[i];
        uint8_t *vr = &r[i << 4];
        int bit = 1 << i;
        int pitch = (vr[2] | (vr[3] & 0x3F) << 8);
        if ((r[PMON] & bit & 0xFE)) pitch += ((prev_out >> 5) * pitch) >> 10;
        int out;
        if (v->kon_delay) {
            if (v->kon_delay == 5) {
                int e = (uint16_t)(dir + vr[4] * 4);
                v->brr_addr = s->ram[e] | s->ram[(uint16_t)(e + 1)] << 8;
                v->brr_offset = 1;
                v->buf_pos = 0;
            }
            v->env = 0;
            v->hidden_env = 0;
            v->interp_pos = 0;
            if (--v->kon_delay & 3) v->interp_pos = 0x4000;
            pitch = 0;
            out = 0;
        } else {
            int smp = (r[NON] & bit) ? (int16_t)(d->noise * 2) : interpolate(v);
            out = ((smp * v->env) >> 11) & ~1;
            envelope(d, v, vr);
        }
        if (r[FLG] & 0x80) v->env_mode = ENV_RELEASE, v->env = 0;
        if (d->every_other) {
            if (d->koff & bit) v->env_mode = ENV_RELEASE;
            if (d->kon & bit) {
                v->kon_delay = 5;
                v->env_mode = ENV_ATTACK;
                r[ENDX] &= (uint8_t)~bit;
            }
        }
        /* the next 4 samples once these are used up */
        if (v->interp_pos >= 0x4000) {
            decode_brr(s, v);
            if ((v->brr_offset += 2) >= 9) {
                int hdr = s->ram[(uint16_t)v->brr_addr];
                if (hdr & 1) {
                    int e = (uint16_t)(dir + vr[4] * 4 + 2);
                    v->brr_addr = s->ram[e] | s->ram[(uint16_t)(e + 1)] << 8;
                    r[ENDX] |= (uint8_t)bit;
                    if (!(hdr & 2)) v->env_mode = ENV_RELEASE, v->env = 0;
                } else {
                    v->brr_addr = (uint16_t)(v->brr_addr + 9);
                }
                v->brr_offset = 1;
            }
        }
        v->interp_pos = (v->interp_pos & 0x3FFF) + pitch;
        if (v->interp_pos > 0x7FFF) v->interp_pos = 0x7FFF;
        v->out = out;
        prev_out = out;
        vr[8] = (uint8_t)(v->env >> 4);
        vr[9] = (uint8_t)(out >> 8);
        int al = (out * (int8_t)vr[0]) >> 7, ar = (out * (int8_t)vr[1]) >> 7;
        main_l = clamp16(main_l + al), main_r = clamp16(main_r + ar);
        if (r[EON] & bit) echo_l = clamp16(echo_l + al), echo_r = clamp16(echo_r + ar);
    }

    /* echo: the buffer's sample into the history, the FIR over the last 8 */
    uint16_t ea = (uint16_t)((r[ESA] << 8) + d->echo_offset);
    d->echo_pos = (d->echo_pos + 1) & 7;
    for (int c = 0; c < 2; c++) {
        int16_t e = (int16_t)(s->ram[(uint16_t)(ea + 2 * c)] | s->ram[(uint16_t)(ea + 2 * c + 1)] << 8);
        d->echo_hist[d->echo_pos][c] = d->echo_hist[d->echo_pos + 8][c] = (int16_t)(e >> 1);
    }
    int fir[2];
    for (int c = 0; c < 2; c++) {
        int sum = 0;
        for (int k = 0; k < 7; k++) sum += (d->echo_hist[d->echo_pos + 1 + k][c] * (int8_t)r[k << 4 | 0x0F]) >> 6;
        sum = (int16_t)sum;
        sum += (int16_t)((d->echo_hist[d->echo_pos + 8][c] * (int8_t)r[0x7F]) >> 6);
        fir[c] = clamp16(sum) & ~1;
    }
    int ol = clamp16(((main_l * (int8_t)r[MVOLL]) >> 7) + ((fir[0] * (int8_t)r[EVOLL]) >> 7));
    int orr = clamp16(((main_r * (int8_t)r[MVOLR]) >> 7) + ((fir[1] * (int8_t)r[EVOLR]) >> 7));
    if (!(r[FLG] & 0x20)) {
        int fl = clamp16(echo_l + (int16_t)((fir[0] * (int8_t)r[EFB]) >> 7)) & ~1;
        int fr = clamp16(echo_r + (int16_t)((fir[1] * (int8_t)r[EFB]) >> 7)) & ~1;
        s->ram[ea] = (uint8_t)fl, s->ram[(uint16_t)(ea + 1)] = (uint8_t)(fl >> 8);
        s->ram[(uint16_t)(ea + 2)] = (uint8_t)fr, s->ram[(uint16_t)(ea + 3)] = (uint8_t)(fr >> 8);
    }
    if (!d->echo_offset) d->echo_length = (r[EDL] & 0x0F) * 0x800;
    d->echo_offset += 4;
    if (d->echo_offset >= d->echo_length) d->echo_offset = 0;
    if (r[FLG] & 0x40) ol = orr = 0;

    if (s->nsamples + 2 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1 << 16;
        s->samples = realloc(s->samples, s->cap * sizeof *s->samples);
    }
    s->samples[s->nsamples++] = (int16_t)ol;
    s->samples[s->nsamples++] = (int16_t)orr;
}
