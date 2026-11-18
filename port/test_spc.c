/* Unit tests for the sound side (port/spc.c, port/spc_dsp.c): small SPC700
 * programs, timers, the boot ROM protocol, the shadow feed, the DSP. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spc.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); checks++; \
    if (_a != _b) { fails++; printf("  FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static Spc S;

/* A machine with the boot ROM done and code at $0200. */
static Spc *at(const uint8_t *code, size_t n)
{
    spc_init(&S);
    S.ipl = 5;  /* IPL_OFF */
    S.control = 0;
    S.sp = 0xEF;
    memcpy(S.ram + 0x200, code, n);
    S.pc = 0x200;
    return &S;
}
static void run_to(Spc *s, uint16_t pc, int max)
{
    for (int i = 0; i < max && s->pc != pc; i++) spc_step(s);
}

static void test_alu(void)
{
    printf("alu\n");
    /* mov a,#$7F; clrc; adc a,#$01 -> $80, V N H set, C clear */
    static const uint8_t p1[] = {0xE8, 0x7F, 0x60, 0x88, 0x01};
    Spc *s = at(p1, sizeof p1);
    run_to(s, 0x205, 10);
    CHECK_EQ(s->a, 0x80);
    CHECK((s->psw & 0x40) && (s->psw & 0x80) && (s->psw & 0x08) && !(s->psw & 1));
    /* mov a,#$10; setc; sbc a,#$20 -> $F0, C clear (borrow) */
    static const uint8_t p2[] = {0xE8, 0x10, 0x80, 0xA8, 0x20};
    s = at(p2, sizeof p2);
    run_to(s, 0x205, 10);
    CHECK_EQ(s->a, 0xF0);
    CHECK(!(s->psw & 1));
    /* YA = $1234 / X = $56 -> A = $36, Y = $04 */
    static const uint8_t p3[] = {0x8D, 0x12, 0xE8, 0x34, 0xCD, 0x56, 0x9E};
    s = at(p3, sizeof p3);
    run_to(s, 0x207, 10);
    CHECK_EQ(s->a, 0x1234 / 0x56);
    CHECK_EQ(s->y, 0x1234 % 0x56);
    /* mul: Y=$12 A=$34 -> YA = $03A8 */
    static const uint8_t p4[] = {0x8D, 0x12, 0xE8, 0x34, 0xCF};
    s = at(p4, sizeof p4);
    run_to(s, 0x205, 10);
    CHECK_EQ(s->y << 8 | s->a, 0x12 * 0x34);
    /* movw ya,$10 ($10 = $FFFF); addw ya,$12 ($12 = $0002) -> $0001, C */
    static const uint8_t p5[] = {0xBA, 0x10, 0x7A, 0x12};
    s = at(p5, sizeof p5);
    s->ram[0x10] = 0xFF, s->ram[0x11] = 0xFF, s->ram[0x12] = 2, s->ram[0x13] = 0;
    run_to(s, 0x204, 10);
    CHECK_EQ(s->y << 8 | s->a, 1);
    CHECK(s->psw & 1);
    /* daa: $19 + $28 = $41, adjusted to $47 */
    static const uint8_t p6[] = {0xE8, 0x19, 0x60, 0x88, 0x28, 0xDF};
    s = at(p6, sizeof p6);
    run_to(s, 0x206, 10);
    CHECK_EQ(s->a, 0x47);
}

static void test_flow(void)
{
    printf("flow\n");
    /* mov y,#3; loop: inc a; dbnz y,loop -> A = 3, cycles 2 + (2+6)*2 + (2+4) */
    static const uint8_t p1[] = {0x8D, 0x03, 0xBC, 0xFE, 0xFD};
    Spc *s = at(p1, sizeof p1);
    run_to(s, 0x205, 20);
    CHECK_EQ(s->a, 3);
    CHECK_EQ(s->cycles, 2 + (2 + 6) * 2 + (2 + 4));
    /* call $0300 (inc x; ret); then pcall / tcall land where they point */
    static const uint8_t p2[] = {0x3F, 0x00, 0x03, 0x3D};
    s = at(p2, sizeof p2);
    s->ram[0x300] = 0x3D, s->ram[0x301] = 0x6F;
    run_to(s, 0x204, 10);
    CHECK_EQ(s->x, 2);
    CHECK_EQ(s->sp, 0xEF);
    /* tcall 1 reads its vector at $FFDC */
    static const uint8_t p3[] = {0x11};
    s = at(p3, sizeof p3);
    s->ram[0xFFDC] = 0x00, s->ram[0xFFDD] = 0x04;
    spc_step(s);
    CHECK_EQ(s->pc, 0x400);
    /* bbs $20.3 taken; cbne not taken when equal */
    static const uint8_t p4[] = {0x63, 0x20, 0x02, 0x00, 0x00, 0x2E, 0x21, 0x10};
    s = at(p4, sizeof p4);
    s->ram[0x20] = 0x08, s->ram[0x21] = 0, s->a = 0;
    spc_step(s);
    CHECK_EQ(s->pc, 0x205);
    spc_step(s);
    CHECK_EQ(s->pc, 0x208);
    /* direct page 1 (setp): mov $10,#$5A writes $0110 */
    static const uint8_t p5[] = {0x40, 0x8F, 0x5A, 0x10};
    s = at(p5, sizeof p5);
    run_to(s, 0x204, 10);
    CHECK_EQ(s->ram[0x110], 0x5A);
    CHECK_EQ(s->ram[0x10], 0);
}

static void test_timers(void)
{
    printf("timers\n");
    /* T0 target 2: one tick of $FD per 2 * 128 cycles; reading clears it */
    static const uint8_t p[] = {0x2F, 0xFE};  /* bra self */
    Spc *s = at(p, sizeof p);
    s->ram[0xFA] = 2, s->target[0] = 2;
    s->control = 0;
    /* (through the bus, as code would) */
    static const uint8_t en[] = {0x8F, 0x02, 0xFA, 0x8F, 0x01, 0xF1, 0x2F, 0xFE};
    s = at(en, sizeof en);
    run_to(s, 0x206, 10);
    uint64_t c0 = s->cycles;
    while (s->cycles < c0 + 2 * 128 * 3 + 10) spc_step(s);
    CHECK_EQ(s->tout[0], 3);
    static const uint8_t rd[] = {0xE4, 0xFD, 0xE4, 0xFD};
    memcpy(s->ram + 0x300, rd, sizeof rd);
    s->pc = 0x300;
    spc_step(s);
    CHECK_EQ(s->a, 3);
    spc_step(s);
    CHECK_EQ(s->a, 0);
}

/* The CPU's side of the boot ROM protocol, as the game does it. */
static void cpu_upload(Spc *s, uint16_t dest, const uint8_t *d, int n, uint16_t run)
{
    spc_run_until(s, s->cycles + 2000);
    CHECK_EQ(s->out[0], 0xAA);
    CHECK_EQ(s->out[1], 0xBB);
    s->in[1] = 1, s->in[2] = (uint8_t)dest, s->in[3] = (uint8_t)(dest >> 8), s->in[0] = 0xCC;
    spc_run_until(s, s->cycles + 200);
    CHECK_EQ(s->out[0], 0xCC);
    for (int i = 0; i < n; i++) {
        s->in[1] = d[i], s->in[0] = (uint8_t)i;
        for (int k = 0; k < 100 && s->out[0] != (uint8_t)i; k++) spc_step(s);
        CHECK_EQ(s->out[0], (uint8_t)i);
    }
    /* run: address, port 1 = 0, port 0 = last + 2 */
    s->in[1] = 0, s->in[2] = (uint8_t)run, s->in[3] = (uint8_t)(run >> 8), s->in[0] = (uint8_t)(n + 1);
    for (int k = 0; k < 100 && s->pc != run; k++) spc_step(s);
}

static void test_ipl(void)
{
    printf("boot rom\n");
    spc_init(&S);
    /* mov $F5,#$42; bra self */
    static const uint8_t code[] = {0x8F, 0x42, 0xF5, 0x2F, 0xFE};
    cpu_upload(&S, 0x0400, code, sizeof code, 0x0400);
    CHECK_EQ(S.pc, 0x0400);
    CHECK(!memcmp(S.ram + 0x400, code, sizeof code));
    spc_step(&S);
    CHECK_EQ(S.out[1], 0x42);
    /* a jump back to $FFC0 (ROM on) starts the protocol over */
    S.control = 0x80;
    S.pc = 0xFFC0;
    spc_run_until(&S, S.cycles + 2000);
    CHECK_EQ(S.out[0], 0xAA);
}

static void test_feed(void)
{
    printf("feed\n");
    /* A sync holds later writes until the SPC's port shows the value. */
    static const uint8_t p[] = {0x2F, 0xFE};
    Spc *s = at(p, sizeof p);
    spc_feed_write(s, 0, 1, 0x11);
    spc_feed_sync(s, 0, 0, 0x99);
    spc_feed_write(s, 0, 1, 0x22);
    spc_run_until(s, 1000);
    CHECK_EQ(s->in[1], 0x11);
    s->out[0] = 0x99;
    spc_run_until(s, 1100);
    CHECK_EQ(s->in[1], 0x22);
    CHECK_EQ(s->timeouts, 0);
    /* Never shown: let go after the timeout, counted. */
    s = at(p, sizeof p);
    spc_feed_sync(s, 0, 0, 0x77);
    spc_feed_write(s, 0, 1, 0x33);
    spc_run_until(s, 200000);
    CHECK_EQ(s->in[1], 0x33);
    CHECK_EQ(s->timeouts, 1);
    /* A late handshake delays what follows by as much: two writes to a
     * port keep their spacing (the frame-step prefix: the driver missed
     * the game's $FF, overwritten in the same instant by the next). */
    s = at(p, sizeof p);
    spc_feed_sync(s, 100, 0, 0x55);
    spc_feed_write(s, 110, 0, 0xFF);
    spc_feed_write(s, 3110, 0, 0x41);
    spc_run_until(s, 20000);
    s->out[0] = 0x55;   /* the driver answers late */
    spc_run_until(s, 20010);
    CHECK_EQ(s->in[0], 0xFF);
    spc_run_until(s, 20000 + 2500);
    CHECK_EQ(s->in[0], 0xFF);   /* still: 3000 cycles after the $FF */
    spc_run_until(s, 20000 + 3100);
    CHECK_EQ(s->in[0], 0x41);
    /* A write waits for its time. */
    s = at(p, sizeof p);
    spc_feed_write(s, 5000, 2, 0x44);
    spc_run_until(s, 4000);
    CHECK_EQ(s->in[2], 0);
    spc_run_until(s, 6000);
    CHECK_EQ(s->in[2], 0x44);
}

/* DSP: one voice playing a looped BRR block of a constant. */
static void dsp_w(Spc *s, int a, int v) { spc_dsp_write(s, a, (uint8_t)v); }
static Spc *voice_setup(int adsr1, int adsr2, int gain)
{
    static const uint8_t p[] = {0x2F, 0xFE};
    Spc *s = at(p, sizeof p);
    /* directory at $0300: sample 0 starts and loops at $0400 */
    s->ram[0x300] = 0x00, s->ram[0x301] = 0x04, s->ram[0x302] = 0x00, s->ram[0x303] = 0x04;
    /* one block, shift 12, filter 0, loop + end; all nibbles 1 -> 2048 each */
    s->ram[0x400] = 0xC3;
    for (int i = 1; i < 9; i++) s->ram[0x400 + i] = 0x11;
    dsp_w(s, 0x6C, 0x20);   /* FLG: no reset/mute, echo writes off */
    dsp_w(s, 0x5D, 0x03);   /* DIR */
    dsp_w(s, 0x0C, 0x7F), dsp_w(s, 0x1C, 0x7F);
    dsp_w(s, 0x00, 0x7F), dsp_w(s, 0x01, 0x7F);
    dsp_w(s, 0x02, 0x00), dsp_w(s, 0x03, 0x10);  /* pitch $1000: 1 sample per sample */
    dsp_w(s, 0x04, 0);
    dsp_w(s, 0x05, adsr1), dsp_w(s, 0x06, adsr2), dsp_w(s, 0x07, gain);
    return s;
}

static void test_dsp(void)
{
    printf("dsp\n");
    /* GAIN direct $7F: env $7F0; the BRR value 1 << 12 >> 1 = 2048, doubled
     * 4096; out = 4096 * $7F0 >> 11 = 4064, then * 127 >> 7 twice. */
    Spc *s = voice_setup(0x00, 0x00, 0x7F);
    dsp_w(s, 0x4C, 0x01);
    s->nsamples = 0;
    for (int i = 0; i < 40; i++) spc_dsp_sample(s);
    int want = ((4096 * 0x7F0) >> 11) & ~1;
    want = (want * 127) >> 7;
    want = (want * 127) >> 7;
    /* (the 4 Gaussian taps sum to ~1: a constant comes out within a few) */
    CHECK(abs(s->samples[2 * 39] - want) <= 8);
    CHECK_EQ(s->samples[2 * 39 + 1], s->samples[2 * 39]);
    CHECK_EQ(s->dsp.regs[0x08], 0x7F0 >> 4);  /* ENVX */
    CHECK(s->dsp.regs[0x7C] & 1);            /* ENDX: the end block was played */
    /* the first samples are silent: key-on takes 5 samples */
    CHECK_EQ(s->samples[0], 0);
    CHECK_EQ(s->samples[2 * 4], 0);
    /* KOFF: release, env -8 a sample */
    dsp_w(s, 0x5C, 0x01);
    for (int i = 0; i < 4; i++) spc_dsp_sample(s);
    CHECK(s->dsp.v[0].env < 0x7F0 && s->dsp.v[0].env >= 0x7F0 - 8 * 4);

    /* ADSR attack rate 15 (+$400 a sample) reaches $7FF in 2, then decays
     * toward the sustain level */
    s = voice_setup(0x8F, 0x00, 0);
    dsp_w(s, 0x4C, 0x01);
    for (int i = 0; i < 12; i++) spc_dsp_sample(s);
    CHECK_EQ(s->dsp.v[0].env_mode, 2);  /* decay */
    CHECK(s->dsp.v[0].env >= 0x7E0);

    /* FLG mute: silent */
    s = voice_setup(0x00, 0x00, 0x7F);
    dsp_w(s, 0x6C, 0x60);
    dsp_w(s, 0x4C, 0x01);
    s->nsamples = 0;
    for (int i = 0; i < 20; i++) spc_dsp_sample(s);
    CHECK_EQ(s->samples[2 * 19], 0);

    /* Echo: a 1-sample-delay-free buffer (EDL 0, 4 bytes) fed back */
    s = voice_setup(0x00, 0x00, 0x7F);
    dsp_w(s, 0x6C, 0x00);   /* echo writes on */
    dsp_w(s, 0x6D, 0x08);   /* ESA $0800 */
    dsp_w(s, 0x7D, 0x00);
    dsp_w(s, 0x4D, 0x01);   /* EON */
    dsp_w(s, 0x7F, 0x7F);   /* FIR 7 = $7F, the rest 0 */
    dsp_w(s, 0x2C, 0x7F), dsp_w(s, 0x3C, 0x7F);
    dsp_w(s, 0x4C, 0x01);
    for (int i = 0; i < 30; i++) spc_dsp_sample(s);
    int16_t e = (int16_t)(s->ram[0x800] | s->ram[0x801] << 8);
    CHECK(e > 0);  /* the voice reached the echo buffer */
}

/* .spc round trip: what spc_save writes, spc_load reads back. */
static void test_spcfile(void)
{
    printf("spc file\n");
    static const uint8_t p[] = {0x2F, 0xFE};
    Spc *s = at(p, sizeof p);
    s->a = 1, s->x = 2, s->y = 3, s->psw = 0x02;
    s->ram[0x1234] = 0x56;
    s->dsp.regs[0x5D] = 0x12;
    s->target[1] = 9;
    s->control = 0x02;
    static uint8_t buf[0x10200];
    spc_save(s, buf);
    static Spc t;
    CHECK_EQ(spc_load(&t, buf, sizeof buf), 0);
    CHECK_EQ(t.pc, 0x200);
    CHECK_EQ(t.a, 1);
    CHECK_EQ(t.y, 3);
    CHECK_EQ(t.ram[0x1234], 0x56);
    CHECK_EQ(t.dsp.regs[0x5D], 0x12);
    CHECK_EQ(t.target[1], 9);
    CHECK_EQ(t.control, 0x02);
}

int main(void)
{
    test_alu();
    test_flow();
    test_timers();
    test_ipl();
    test_feed();
    test_dsp();
    test_spcfile();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
