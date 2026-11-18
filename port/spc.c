/* SPC700: the sound CPU, its I/O registers and timers, the boot ROM's
 * protocol (high level) and the shadow feed. See spc.h. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "spc.h"

enum { FN = 0x80, FV = 0x40, FP = 0x20, FB = 0x10, FH = 0x08, FI = 0x04, FZ = 0x02, FC = 0x01 };

/* The handshake a sync waits for: past this, it is taken as met (and
 * counted) so a mismatch can't stall the sound for good. */
#define SYNC_TIMEOUT 64000

void spc_init(Spc *s)
{
    int16_t *smp = s->samples;
    size_t cap = s->cap;
    SpcEvent *q = s->q;
    size_t qcap = s->qcap;
    memset(s, 0, sizeof *s);
    s->samples = smp, s->cap = cap;
    s->q = q, s->qcap = qcap;
    s->control = 0x80;
    s->pc = 0xFFC0;
    s->ipl = 0;
    s->dsp.regs[0x6C] = 0xE0;   /* FLG: reset, mute, echo writes off */
    s->dsp.echo_length = 0;
    for (int i = 0; i < 8; i++) s->dsp.v[i].brr_offset = 1, s->dsp.v[i].env_mode = 0;
    s->dsp.noise = 0x4000;
}

/* --- memory --- */

static uint8_t rd(Spc *s, uint16_t a)
{
    if (a >= 0xF0 && a < 0x100) {
        switch (a) {
        case 0xF2: return s->dspaddr;
        case 0xF3: return spc_dsp_read(s, s->dspaddr & 0x7F);
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: return s->in[a - 0xF4];
        case 0xFD: case 0xFE: case 0xFF: {
            uint8_t v = s->tout[a - 0xFD];
            s->tout[a - 0xFD] = 0;
            return v;
        }
        case 0xF8: case 0xF9: return s->ram[a];
        default: return 0;
        }
    }
    return s->ram[a];
}

static void wr(Spc *s, uint16_t a, uint8_t v)
{
    s->ram[a] = v;
    if (a < 0xF0 || a >= 0x100) return;
    switch (a) {
    case 0xF1:
        for (int i = 0; i < 3; i++)
            if ((v >> i & 1) && !(s->control >> i & 1)) s->tcount[i] = 0, s->tout[i] = 0;
        if (v & 0x10) s->in[0] = s->in[1] = 0;
        if (v & 0x20) s->in[2] = s->in[3] = 0;
        s->control = v;
        break;
    case 0xF2: s->dspaddr = v; break;
    case 0xF3: if (s->dspaddr < 0x80) spc_dsp_write(s, s->dspaddr, v); break;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: s->out[a - 0xF4] = v; break;
    case 0xFA: case 0xFB: case 0xFC: s->target[a - 0xFA] = v; break;
    default: break;
    }
}

static uint8_t imm(Spc *s) { return rd(s, s->pc++); }
static uint16_t imm16(Spc *s) { uint16_t l = imm(s); return l | imm(s) << 8; }
static uint16_t dp(Spc *s, uint8_t d) { return (uint16_t)((s->psw & FP ? 0x100 : 0) | d); }
static uint16_t rd16dp(Spc *s, uint8_t d) { return rd(s, dp(s, d)) | rd(s, dp(s, (uint8_t)(d + 1))) << 8; }
static void push(Spc *s, uint8_t v) { wr(s, (uint16_t)(0x100 | s->sp--), v); }
static uint8_t pop(Spc *s) { return rd(s, (uint16_t)(0x100 | ++s->sp)); }
static void nz(Spc *s, uint8_t v) { s->psw = (uint8_t)((s->psw & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ)); }

/* --- ALU --- */

static uint8_t adc(Spc *s, uint8_t a, uint8_t b)
{
    int r = a + b + (s->psw & FC);
    s->psw &= (uint8_t)~(FV | FH | FC);
    if (r > 0xFF) s->psw |= FC;
    if ((a ^ b ^ r) & 0x10) s->psw |= FH;
    if (~(a ^ b) & (a ^ r) & 0x80) s->psw |= FV;
    nz(s, (uint8_t)r);
    return (uint8_t)r;
}
static uint8_t sbc(Spc *s, uint8_t a, uint8_t b) { return adc(s, a, (uint8_t)~b); }
static void cmp(Spc *s, uint8_t a, uint8_t b)
{
    int r = a - b;
    s->psw = (uint8_t)((s->psw & ~FC) | (r >= 0 ? FC : 0));
    nz(s, (uint8_t)r);
}
static uint8_t alu(Spc *s, int op, uint8_t a, uint8_t b)
{
    switch (op) {
    case 0: a |= b; nz(s, a); return a;
    case 1: a &= b; nz(s, a); return a;
    case 2: a ^= b; nz(s, a); return a;
    case 3: cmp(s, a, b); return a;
    case 4: return adc(s, a, b);
    default: return sbc(s, a, b);
    }
}
static uint8_t shift(Spc *s, int op, uint8_t v)
{
    int c = s->psw & FC;
    switch (op) {
    case 0: c = v >> 7; v = (uint8_t)(v << 1); break;            /* ASL */
    case 1: { int o = v >> 7; v = (uint8_t)(v << 1 | c); c = o; break; }  /* ROL */
    case 2: c = v & 1; v >>= 1; break;                             /* LSR */
    default: { int o = v & 1; v = (uint8_t)(v >> 1 | c << 7); c = o; break; }  /* ROR */
    }
    s->psw = (uint8_t)((s->psw & ~FC) | c);
    nz(s, v);
    return v;
}
static int branch(Spc *s, int cond)
{
    int8_t r = (int8_t)imm(s);
    if (!cond) return 2;
    s->pc = (uint16_t)(s->pc + r);
    return 4;
}

/* --- the boot ROM, high level ---
 * On reset (and on a jump to $FFC0) it clears $01-$EF, sets SP $EF, shows $AA/$BB on ports 0/1, waits for
 * $CC on port 0, then: address from ports 2/3, port 1 nonzero = a block
 * follows, zero = jump there. Port 0's value is echoed. In a block, byte
 * i (port 1) is taken when port 0 = i and echoed; port 0 jumping past i
 * (i + 2 or more, the CPU's "next block" kick) starts the next block. */
enum { IPL_INIT, IPL_WAITCC, IPL_KICK, IPL_WAIT0, IPL_DATA, IPL_OFF };

static int ipl_step(Spc *s)
{
    switch (s->ipl) {
    case IPL_INIT:
        for (int i = 1; i <= 0xEF; i++) s->ram[i] = 0;
        s->sp = 0xEF;
        s->a = 0;
        s->x = 0;
        wr(s, 0xF4, 0xAA);
        wr(s, 0xF5, 0xBB);
        s->ipl = IPL_WAITCC;
        return 1000;
    case IPL_WAITCC:
        if (s->in[0] != 0xCC) return 5;
        s->ipl = IPL_KICK;
        return 5;
    case IPL_KICK: {
        s->ipl_dest = (uint16_t)(s->in[2] | s->in[3] << 8);
        s->ram[0] = s->in[2], s->ram[1] = s->in[3];
        uint8_t kick = s->in[0], cmd = s->in[1];
        wr(s, 0xF4, kick);
        s->a = kick, s->y = cmd, s->x = cmd;
        if (!cmd) {
            s->ipl = IPL_OFF;
            s->pc = s->ipl_dest;
            s->x = 0;
            return 12;
        }
        s->ipl = IPL_WAIT0;
        return 16;
    }
    case IPL_WAIT0:
        if (s->in[0] != 0) return 5;
        s->ipl_y = 0;
        s->ipl = IPL_DATA;
        return 5;
    case IPL_DATA: {
        int8_t d = (int8_t)(s->ipl_y - s->in[0]);
        if (d == 0) {
            uint8_t v = s->in[1];
            wr(s, 0xF4, (uint8_t)s->ipl_y);
            wr(s, (uint16_t)(s->ipl_dest + s->ipl_y), v);
            if (++s->ipl_y == 0x100) s->ipl_y = 0, s->ipl_dest += 0x100;
            return 20;
        }
        if (d < 0) s->ipl = IPL_KICK;  /* port 0 ran ahead: a new block */
        return 7;
    }
    }
    return 2;
}

/* --- timers, samples --- */

static void tick(Spc *s, int cyc)
{
    static const int period[3] = {128, 128, 16};
    for (int i = 0; i < 3; i++) {
        s->tdiv[i] += cyc;
        while (s->tdiv[i] >= period[i]) {
            s->tdiv[i] -= period[i];
            if (!(s->control >> i & 1)) continue;
            if (++s->tcount[i] == s->target[i]) s->tcount[i] = 0, s->tout[i] = (s->tout[i] + 1) & 15;
        }
    }
    s->sample_div += cyc;
    while (s->sample_div >= 32) {
        s->sample_div -= 32;
        spc_dsp_sample(s);
    }
    s->cycles += (uint64_t)cyc;
}

/* --- instructions --- */

static uint16_t bitaddr(Spc *s, int *bit)
{
    uint16_t w = imm16(s);
    *bit = w >> 13;
    return w & 0x1FFF;
}

static int exec(Spc *s)
{
    uint8_t op = imm(s), t, u;
    uint16_t a, w;
    int bit;
    /* the ALU block: OR AND EOR CMP ADC SBC at rows 0,2,4,6,8,A (and +1) */
    int hi = op >> 4, lo = op & 15;
    if (hi <= 0xB && lo >= 4 && lo <= 9) {
        int k = hi >> 1, odd = hi & 1;
        if (!odd) {
            switch (lo) {
            case 4: s->a = alu(s, k, s->a, rd(s, dp(s, imm(s)))); return 3;
            case 5: s->a = alu(s, k, s->a, rd(s, imm16(s))); return 4;
            case 6: s->a = alu(s, k, s->a, rd(s, dp(s, s->x))); return 3;
            case 7: t = imm(s); s->a = alu(s, k, s->a, rd(s, rd16dp(s, (uint8_t)(t + s->x)))); return 6;
            case 8: s->a = alu(s, k, s->a, imm(s)); return 2;
            case 9: {
                uint8_t src = rd(s, dp(s, imm(s)));
                a = dp(s, imm(s));
                t = alu(s, k, rd(s, a), src);
                if (k != 3) wr(s, a, t);
                return 6;
            }
            }
        } else {
            switch (lo) {
            case 4: s->a = alu(s, k, s->a, rd(s, dp(s, (uint8_t)(imm(s) + s->x)))); return 4;
            case 5: s->a = alu(s, k, s->a, rd(s, (uint16_t)(imm16(s) + s->x))); return 5;
            case 6: s->a = alu(s, k, s->a, rd(s, (uint16_t)(imm16(s) + s->y))); return 5;
            case 7: t = imm(s); s->a = alu(s, k, s->a, rd(s, (uint16_t)(rd16dp(s, t) + s->y))); return 6;
            case 8: {
                uint8_t i = imm(s);
                a = dp(s, imm(s));
                t = alu(s, k, rd(s, a), i);
                if (k != 3) wr(s, a, t);
                return 5;
            }
            case 9: {
                uint8_t src = rd(s, dp(s, s->y));
                a = dp(s, s->x);
                t = alu(s, k, rd(s, a), src);
                if (k != 3) wr(s, a, t);
                return 5;
            }
            }
        }
    }
    /* TCALL n, SET1/CLR1 d.b, BBS/BBC d.b */
    if (lo == 1) {
        push(s, (uint8_t)(s->pc >> 8));
        push(s, (uint8_t)s->pc);
        a = (uint16_t)(0xFFDE - 2 * hi);
        s->pc = (uint16_t)(rd(s, a) | rd(s, (uint16_t)(a + 1)) << 8);
        return 8;
    }
    if (lo == 2) {
        a = dp(s, imm(s));
        t = rd(s, a);
        bit = hi >> 1;
        wr(s, a, hi & 1 ? (uint8_t)(t & ~(1 << bit)) : (uint8_t)(t | 1 << bit));
        return 4;
    }
    if (lo == 3) {
        t = rd(s, dp(s, imm(s)));
        int8_t r = (int8_t)imm(s);
        int set = t >> (hi >> 1) & 1;
        if ((hi & 1) ? !set : set) { s->pc = (uint16_t)(s->pc + r); return 7; }
        return 5;
    }
    switch (op) {
    case 0x00: return 2;
    case 0x0A: a = bitaddr(s, &bit); if (rd(s, a) >> bit & 1) s->psw |= FC; return 5;          /* OR1 */
    case 0x2A: a = bitaddr(s, &bit); if (!(rd(s, a) >> bit & 1)) s->psw |= FC; return 5;       /* OR1 / */
    case 0x4A: a = bitaddr(s, &bit); if (!(rd(s, a) >> bit & 1)) s->psw &= (uint8_t)~FC; return 4;  /* AND1 */
    case 0x6A: a = bitaddr(s, &bit); if (rd(s, a) >> bit & 1) s->psw &= (uint8_t)~FC; return 4;     /* AND1 / */
    case 0x8A: a = bitaddr(s, &bit); if (rd(s, a) >> bit & 1) s->psw ^= FC; return 5;          /* EOR1 */
    case 0xAA: a = bitaddr(s, &bit); s->psw = (uint8_t)((s->psw & ~FC) | (rd(s, a) >> bit & 1)); return 4;  /* MOV1 C,m.b */
    case 0xCA: a = bitaddr(s, &bit); t = rd(s, a);                                               /* MOV1 m.b,C */
        wr(s, a, (s->psw & FC) ? (uint8_t)(t | 1 << bit) : (uint8_t)(t & ~(1 << bit))); return 6;
    case 0xEA: a = bitaddr(s, &bit); wr(s, a, (uint8_t)(rd(s, a) ^ 1 << bit)); return 5;       /* NOT1 */

    case 0x0B: a = dp(s, imm(s)); wr(s, a, shift(s, 0, rd(s, a))); return 4;
    case 0x2B: a = dp(s, imm(s)); wr(s, a, shift(s, 1, rd(s, a))); return 4;
    case 0x4B: a = dp(s, imm(s)); wr(s, a, shift(s, 2, rd(s, a))); return 4;
    case 0x6B: a = dp(s, imm(s)); wr(s, a, shift(s, 3, rd(s, a))); return 4;
    case 0x0C: a = imm16(s); wr(s, a, shift(s, 0, rd(s, a))); return 5;
    case 0x2C: a = imm16(s); wr(s, a, shift(s, 1, rd(s, a))); return 5;
    case 0x4C: a = imm16(s); wr(s, a, shift(s, 2, rd(s, a))); return 5;
    case 0x6C: a = imm16(s); wr(s, a, shift(s, 3, rd(s, a))); return 5;
    case 0x1B: a = dp(s, (uint8_t)(imm(s) + s->x)); wr(s, a, shift(s, 0, rd(s, a))); return 5;
    case 0x3B: a = dp(s, (uint8_t)(imm(s) + s->x)); wr(s, a, shift(s, 1, rd(s, a))); return 5;
    case 0x5B: a = dp(s, (uint8_t)(imm(s) + s->x)); wr(s, a, shift(s, 2, rd(s, a))); return 5;
    case 0x7B: a = dp(s, (uint8_t)(imm(s) + s->x)); wr(s, a, shift(s, 3, rd(s, a))); return 5;
    case 0x1C: s->a = shift(s, 0, s->a); return 2;
    case 0x3C: s->a = shift(s, 1, s->a); return 2;
    case 0x5C: s->a = shift(s, 2, s->a); return 2;
    case 0x7C: s->a = shift(s, 3, s->a); return 2;

    case 0x8B: a = dp(s, imm(s)); t = (uint8_t)(rd(s, a) - 1); wr(s, a, t); nz(s, t); return 4;
    case 0xAB: a = dp(s, imm(s)); t = (uint8_t)(rd(s, a) + 1); wr(s, a, t); nz(s, t); return 4;
    case 0x8C: a = imm16(s); t = (uint8_t)(rd(s, a) - 1); wr(s, a, t); nz(s, t); return 5;
    case 0xAC: a = imm16(s); t = (uint8_t)(rd(s, a) + 1); wr(s, a, t); nz(s, t); return 5;
    case 0x9B: a = dp(s, (uint8_t)(imm(s) + s->x)); t = (uint8_t)(rd(s, a) - 1); wr(s, a, t); nz(s, t); return 5;
    case 0xBB: a = dp(s, (uint8_t)(imm(s) + s->x)); t = (uint8_t)(rd(s, a) + 1); wr(s, a, t); nz(s, t); return 5;
    case 0x9C: nz(s, --s->a); return 2;
    case 0xBC: nz(s, ++s->a); return 2;
    case 0x1D: nz(s, --s->x); return 2;
    case 0x3D: nz(s, ++s->x); return 2;
    case 0xDC: nz(s, --s->y); return 2;
    case 0xFC: nz(s, ++s->y); return 2;

    case 0x0D: push(s, s->psw); return 4;
    case 0x2D: push(s, s->a); return 4;
    case 0x4D: push(s, s->x); return 4;
    case 0x6D: push(s, s->y); return 4;
    case 0x8E: s->psw = pop(s); return 4;
    case 0xAE: s->a = pop(s); return 4;
    case 0xCE: s->x = pop(s); return 4;
    case 0xEE: s->y = pop(s); return 4;

    case 0x0E: case 0x4E: {                                     /* TSET1 / TCLR1 !a */
        a = imm16(s);
        t = rd(s, a);
        nz(s, (uint8_t)(s->a - t));
        wr(s, a, op == 0x0E ? (uint8_t)(t | s->a) : (uint8_t)(t & ~s->a));
        return 6;
    }
    case 0x0F:                                                  /* BRK */
        push(s, (uint8_t)(s->pc >> 8));
        push(s, (uint8_t)s->pc);
        push(s, s->psw);
        s->psw = (uint8_t)((s->psw | FB) & ~FI);
        s->pc = (uint16_t)(rd(s, 0xFFDE) | rd(s, 0xFFDF) << 8);
        return 8;
    case 0x10: return branch(s, !(s->psw & FN));
    case 0x30: return branch(s, s->psw & FN);
    case 0x50: return branch(s, !(s->psw & FV));
    case 0x70: return branch(s, s->psw & FV);
    case 0x90: return branch(s, !(s->psw & FC));
    case 0xB0: return branch(s, s->psw & FC);
    case 0xD0: return branch(s, !(s->psw & FZ));
    case 0xF0: return branch(s, s->psw & FZ);
    case 0x2F: return branch(s, 1);

    case 0x1A: case 0x3A: {                                     /* DECW / INCW d */
        t = imm(s);
        w = (uint16_t)(rd16dp(s, t) + (op == 0x3A ? 1 : -1));
        wr(s, dp(s, t), (uint8_t)w);
        wr(s, dp(s, (uint8_t)(t + 1)), (uint8_t)(w >> 8));
        s->psw = (uint8_t)((s->psw & ~(FN | FZ)) | (w & 0x8000 ? FN : 0) | (w ? 0 : FZ));
        return 6;
    }
    case 0x5A: {                                                /* CMPW YA,d */
        w = rd16dp(s, imm(s));
        int ya = s->y << 8 | s->a, r = ya - w;
        s->psw = (uint8_t)((s->psw & ~(FN | FZ | FC)) | (r & 0x8000 ? FN : 0) | ((r & 0xFFFF) ? 0 : FZ) | (r >= 0 ? FC : 0));
        return 4;
    }
    case 0x7A: case 0x9A: {                                     /* ADDW / SUBW YA,d */
        w = rd16dp(s, imm(s));
        int ya = s->y << 8 | s->a, r;
        s->psw &= (uint8_t)~(FN | FZ | FC | FV | FH);
        if (op == 0x7A) {
            r = ya + w;
            if (r > 0xFFFF) s->psw |= FC;
            if (~(ya ^ w) & (ya ^ r) & 0x8000) s->psw |= FV;
            if ((ya ^ w ^ r) & 0x1000) s->psw |= FH;
        } else {
            r = ya - w;
            if (r >= 0) s->psw |= FC;
            if ((ya ^ w) & (ya ^ r) & 0x8000) s->psw |= FV;
            if (!((ya ^ w ^ r) & 0x1000)) s->psw |= FH;
        }
        r &= 0xFFFF;
        if (r & 0x8000) s->psw |= FN;
        if (!r) s->psw |= FZ;
        s->a = (uint8_t)r, s->y = (uint8_t)(r >> 8);
        return 5;
    }
    case 0xBA:                                                  /* MOVW YA,d */
        w = rd16dp(s, imm(s));
        s->a = (uint8_t)w, s->y = (uint8_t)(w >> 8);
        s->psw = (uint8_t)((s->psw & ~(FN | FZ)) | (w & 0x8000 ? FN : 0) | (w ? 0 : FZ));
        return 5;
    case 0xDA:                                                  /* MOVW d,YA */
        t = imm(s);
        wr(s, dp(s, t), s->a);
        wr(s, dp(s, (uint8_t)(t + 1)), s->y);
        return 5;

    case 0x1E: cmp(s, s->x, rd(s, imm16(s))); return 4;
    case 0x3E: cmp(s, s->x, rd(s, dp(s, imm(s)))); return 3;
    case 0xC8: cmp(s, s->x, imm(s)); return 2;
    case 0x5E: cmp(s, s->y, rd(s, imm16(s))); return 4;
    case 0x7E: cmp(s, s->y, rd(s, dp(s, imm(s)))); return 3;
    case 0xAD: cmp(s, s->y, imm(s)); return 2;

    case 0x1F: a = (uint16_t)(imm16(s) + s->x); s->pc = (uint16_t)(rd(s, a) | rd(s, (uint16_t)(a + 1)) << 8); return 6;
    case 0x5F: s->pc = imm16(s); return 3;
    case 0x3F: a = imm16(s); push(s, (uint8_t)(s->pc >> 8)); push(s, (uint8_t)s->pc); s->pc = a; return 8;
    case 0x4F: t = imm(s); push(s, (uint8_t)(s->pc >> 8)); push(s, (uint8_t)s->pc); s->pc = (uint16_t)(0xFF00 | t); return 6;
    case 0x6F: t = pop(s); s->pc = (uint16_t)(t | pop(s) << 8); return 5;
    case 0x7F: s->psw = pop(s); t = pop(s); s->pc = (uint16_t)(t | pop(s) << 8); return 6;

    case 0x20: s->psw &= (uint8_t)~FP; return 2;
    case 0x40: s->psw |= FP; return 2;
    case 0x60: s->psw &= (uint8_t)~FC; return 2;
    case 0x80: s->psw |= FC; return 2;
    case 0xA0: s->psw |= FI; return 3;
    case 0xC0: s->psw &= (uint8_t)~FI; return 3;
    case 0xE0: s->psw &= (uint8_t)~(FV | FH); return 2;
    case 0xED: s->psw ^= FC; return 3;

    case 0x2E: {                                                /* CBNE d,r */
        t = rd(s, dp(s, imm(s)));
        int8_t r = (int8_t)imm(s);
        if (t != s->a) { s->pc = (uint16_t)(s->pc + r); return 7; }
        return 5;
    }
    case 0xDE: {                                                /* CBNE d+X,r */
        t = rd(s, dp(s, (uint8_t)(imm(s) + s->x)));
        int8_t r = (int8_t)imm(s);
        if (t != s->a) { s->pc = (uint16_t)(s->pc + r); return 8; }
        return 6;
    }
    case 0x6E: {                                                /* DBNZ d,r */
        a = dp(s, imm(s));
        t = (uint8_t)(rd(s, a) - 1);
        wr(s, a, t);
        int8_t r = (int8_t)imm(s);
        if (t) { s->pc = (uint16_t)(s->pc + r); return 7; }
        return 5;
    }
    case 0xFE: {                                                /* DBNZ Y,r */
        int8_t r = (int8_t)imm(s);
        if (--s->y) { s->pc = (uint16_t)(s->pc + r); return 6; }
        return 4;
    }

    case 0x8D: s->y = imm(s); nz(s, s->y); return 2;
    case 0xCD: s->x = imm(s); nz(s, s->x); return 2;
    case 0xE8: s->a = imm(s); nz(s, s->a); return 2;
    case 0x5D: s->x = s->a; nz(s, s->x); return 2;
    case 0x7D: s->a = s->x; nz(s, s->a); return 2;
    case 0xDD: s->a = s->y; nz(s, s->a); return 2;
    case 0xFD: s->y = s->a; nz(s, s->y); return 2;
    case 0x9D: s->x = s->sp; nz(s, s->x); return 2;
    case 0xBD: s->sp = s->x; return 2;

    case 0xE4: s->a = rd(s, dp(s, imm(s))); nz(s, s->a); return 3;
    case 0xE5: s->a = rd(s, imm16(s)); nz(s, s->a); return 4;
    case 0xE6: s->a = rd(s, dp(s, s->x)); nz(s, s->a); return 3;
    case 0xE7: t = imm(s); s->a = rd(s, rd16dp(s, (uint8_t)(t + s->x))); nz(s, s->a); return 6;
    case 0xF4: s->a = rd(s, dp(s, (uint8_t)(imm(s) + s->x))); nz(s, s->a); return 4;
    case 0xF5: s->a = rd(s, (uint16_t)(imm16(s) + s->x)); nz(s, s->a); return 5;
    case 0xF6: s->a = rd(s, (uint16_t)(imm16(s) + s->y)); nz(s, s->a); return 5;
    case 0xF7: t = imm(s); s->a = rd(s, (uint16_t)(rd16dp(s, t) + s->y)); nz(s, s->a); return 6;
    case 0xBF: s->a = rd(s, dp(s, s->x++)); nz(s, s->a); return 4;
    case 0xE9: s->x = rd(s, imm16(s)); nz(s, s->x); return 4;
    case 0xF8: s->x = rd(s, dp(s, imm(s))); nz(s, s->x); return 3;
    case 0xF9: s->x = rd(s, dp(s, (uint8_t)(imm(s) + s->y))); nz(s, s->x); return 4;
    case 0xEB: s->y = rd(s, dp(s, imm(s))); nz(s, s->y); return 3;
    case 0xEC: s->y = rd(s, imm16(s)); nz(s, s->y); return 4;
    case 0xFB: s->y = rd(s, dp(s, (uint8_t)(imm(s) + s->x))); nz(s, s->y); return 4;

    case 0xC4: wr(s, dp(s, imm(s)), s->a); return 4;
    case 0xC5: wr(s, imm16(s), s->a); return 5;
    case 0xC6: wr(s, dp(s, s->x), s->a); return 4;
    case 0xC7: t = imm(s); wr(s, rd16dp(s, (uint8_t)(t + s->x)), s->a); return 7;
    case 0xD4: wr(s, dp(s, (uint8_t)(imm(s) + s->x)), s->a); return 5;
    case 0xD5: wr(s, (uint16_t)(imm16(s) + s->x), s->a); return 6;
    case 0xD6: wr(s, (uint16_t)(imm16(s) + s->y), s->a); return 6;
    case 0xD7: t = imm(s); wr(s, (uint16_t)(rd16dp(s, t) + s->y), s->a); return 7;
    case 0xAF: wr(s, dp(s, s->x++), s->a); return 4;
    case 0xC9: wr(s, imm16(s), s->x); return 5;
    case 0xD8: wr(s, dp(s, imm(s)), s->x); return 4;
    case 0xD9: wr(s, dp(s, (uint8_t)(imm(s) + s->y)), s->x); return 5;
    case 0xCC: wr(s, imm16(s), s->y); return 5;
    case 0xCB: wr(s, dp(s, imm(s)), s->y); return 4;
    case 0xDB: wr(s, dp(s, (uint8_t)(imm(s) + s->x)), s->y); return 5;
    case 0xFA: t = rd(s, dp(s, imm(s))); wr(s, dp(s, imm(s)), t); return 5;
    case 0x8F: t = imm(s); wr(s, dp(s, imm(s)), t); return 5;

    case 0xCF: w = (uint16_t)(s->y * s->a); s->a = (uint8_t)w, s->y = (uint8_t)(w >> 8); nz(s, s->y); return 9;
    case 0x9E: {                                                /* DIV YA,X */
        unsigned ya = (unsigned)(s->y << 8 | s->a), x = s->x;
        s->psw &= (uint8_t)~(FV | FH);
        if ((x & 15) <= (s->y & 15)) s->psw |= FH;
        if (s->y >= x) s->psw |= FV;
        if (s->y < (x << 1)) {
            s->a = (uint8_t)(ya / x);
            s->y = (uint8_t)(ya % x);
        } else {
            s->a = (uint8_t)(255 - (ya - (x << 9)) / (256 - x));
            s->y = (uint8_t)(x + (ya - (x << 9)) % (256 - x));
        }
        nz(s, s->a);
        return 12;
    }
    case 0x9F: s->a = (uint8_t)(s->a >> 4 | s->a << 4); nz(s, s->a); return 5;
    case 0xDF:                                                  /* DAA */
        if ((s->psw & FC) || s->a > 0x99) s->a += 0x60, s->psw |= FC;
        if ((s->psw & FH) || (s->a & 15) > 9) s->a += 6;
        nz(s, s->a);
        return 3;
    case 0xBE:                                                  /* DAS */
        if (!(s->psw & FC) || s->a > 0x99) s->a -= 0x60, s->psw &= (uint8_t)~FC;
        if (!(s->psw & FH) || (s->a & 15) > 9) s->a -= 6;
        nz(s, s->a);
        return 3;
    case 0xEF: case 0xFF: s->halted = 1; s->pc--; return 3;
    }
    (void)u;
    return 2;
}

int spc_step(Spc *s)
{
    int c;
    if (s->ipl != IPL_OFF) c = ipl_step(s);
    else if (s->pc == 0xFFC0 && (s->control & 0x80)) { s->ipl = IPL_INIT; c = 2; }
    else if (s->halted) c = 2;
    else c = exec(s);
    tick(s, c);
    return c;
}

/* --- the feed --- */

static void enqueue(Spc *s, uint64_t t, int sync, int p, uint8_t v)
{
    if (s->qhead + s->qlen == s->qcap) {
        if (s->qhead) {
            memmove(s->q, s->q + s->qhead, s->qlen * sizeof *s->q);
            s->qhead = 0;
        }
        if (s->qlen == s->qcap) {
            s->qcap = s->qcap ? s->qcap * 2 : 4096;
            s->q = realloc(s->q, s->qcap * sizeof *s->q);
        }
    }
    s->q[s->qhead + s->qlen++] = (SpcEvent){t, (uint8_t)sync, (uint8_t)p, v};
}
void spc_feed_write(Spc *s, uint64_t t, int p, uint8_t v) { enqueue(s, t, 0, p, v); }
void spc_feed_sync(Spc *s, uint64_t t, int p, uint8_t v) { enqueue(s, t, 1, p, v); }

/* Events keep the CPU's spacing: a handshake the driver meets later than
 * the CPU's fake APU did delays everything after it by as much (s->delay),
 * else two writes to a port a driver polls now and then could land
 * together and it would only see the second (the frame-step prefix: the
 * fake answers at once, and the game's $FF "back to the boot ROM" went
 * unseen). Once the feed is idle a while, the delay starts over. */
static void apply(Spc *s)
{
    if (!s->qlen && s->delay && s->cycles - s->last_apply >= 4096) s->delay = 0;
    while (s->qlen) {
        SpcEvent *e = &s->q[s->qhead];
        if (e->t + s->delay > s->cycles) return;
        if (e->sync) {
            if (s->out[e->port] != e->val) {
                /* (wait_since: the cycle the wait began, + 1; 0 = none) */
                if (!s->wait_since) s->wait_since = s->cycles + 1;
                if (s->cycles + 1 - s->wait_since < SYNC_TIMEOUT) return;
                s->timeouts++;
                if (s->debug && s->timeouts <= 12) {
                    fprintf(stderr, "spc: sync p%d=%02X timed out at %llu (out %02X %02X %02X %02X, in %02X %02X %02X %02X, pc %04X ipl %d); next:",
                            e->port, e->val, (unsigned long long)s->cycles, s->out[0], s->out[1], s->out[2], s->out[3],
                            s->in[0], s->in[1], s->in[2], s->in[3], s->pc, s->ipl);
                    for (size_t k = 1; k < 12 && k < s->qlen; k++)
                        fprintf(stderr, " %s%d=%02X", e[k].sync ? "S" : "W", e[k].port, e[k].val);
                    fprintf(stderr, "\n");
                    const char *dp = getenv("SBM2_SPCDUMP");
                    if (dp && s->timeouts == 1) { FILE *f = fopen(dp, "wb"); if (f) fwrite(s->ram, 1, 0x10000, f), fclose(f); }
                }
            }
            s->syncs++;
            s->wait_since = 0;
        } else {
            s->in[e->port] = e->val;
        }
        if (s->cycles - e->t > s->delay) s->delay = s->cycles - e->t;
        if (s->delay > s->max_lag) s->max_lag = s->delay;
        s->last_apply = s->cycles;
        s->qhead++, s->qlen--;
    }
    s->qhead = 0;
}

void spc_run_until(Spc *s, uint64_t t)
{
    while (s->cycles < t) {
        apply(s);
        spc_step(s);
    }
    apply(s);
}

size_t spc_drain(Spc *s, uint64_t max)
{
    uint64_t end = s->cycles + max;
    while (s->qlen && s->cycles < end) {
        apply(s);
        spc_step(s);
    }
    return s->qlen;
}

/* --- .spc files --- */

size_t spc_save(const Spc *s, uint8_t *out)
{
    memset(out, 0, 0x10200);
    memcpy(out, "SNES-SPC700 Sound File Data v0.30", 33);
    out[0x21] = 26, out[0x22] = 26, out[0x23] = 27, out[0x24] = 30;
    out[0x25] = (uint8_t)s->pc, out[0x26] = (uint8_t)(s->pc >> 8);
    out[0x27] = s->a, out[0x28] = s->x, out[0x29] = s->y, out[0x2A] = s->psw, out[0x2B] = s->sp;
    memcpy(out + 0x100, s->ram, 0x10000);
    uint8_t *io = out + 0x100 + 0xF0;
    io[1] = s->control, io[2] = s->dspaddr;
    for (int i = 0; i < 4; i++) io[4 + i] = s->in[i];
    for (int i = 0; i < 3; i++) io[0xA + i] = s->target[i], io[0xD + i] = s->tout[i];
    memcpy(out + 0x10100, s->dsp.regs, 128);
    memcpy(out + 0x101C0, s->ram + 0xFFC0, 64);
    return 0x10200;
}

int spc_load(Spc *s, const uint8_t *d, size_t n)
{
    if (n < 0x10180 || memcmp(d, "SNES-SPC700 Sound File Data", 27)) return -1;
    spc_init(s);
    s->pc = (uint16_t)(d[0x25] | d[0x26] << 8);
    s->a = d[0x27], s->x = d[0x28], s->y = d[0x29], s->psw = d[0x2A], s->sp = d[0x2B];
    memcpy(s->ram, d + 0x100, 0x10000);
    const uint8_t *io = d + 0x100 + 0xF0;
    s->control = io[1] & 0x87, s->dspaddr = io[2];
    for (int i = 0; i < 4; i++) s->in[i] = io[4 + i], s->out[i] = io[4 + i];
    for (int i = 0; i < 3; i++) s->target[i] = io[0xA + i], s->tout[i] = io[0xD + i] & 15;
    s->ipl = IPL_OFF;
    for (int i = 0; i < 128; i++) {
        if (i == 0x4C) continue;  /* KON: below, as a write */
        s->dsp.regs[i] = d[0x10100 + i];
    }
    s->dsp.echo_length = (s->dsp.regs[0x7D] & 15) * 0x800;
    for (int v = 0; v < 8; v++) s->dsp.v[v].env_mode = 0, s->dsp.v[v].env = 0;
    s->dsp.new_kon = d[0x10100 + 0x4C];
    return 0;
}
