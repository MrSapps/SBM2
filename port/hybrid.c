/* Twin verifier for the hybrid port: two machines in frame-step mode from
 * power-on with the same input, A pure interpreter, B with the native C
 * routines (hooks) enabled. After every logic frame (wait-loop exit) their
 * WRAM, CPU registers, PPU and DMA registers must be identical.
 *
 * Usage: hybrid <rom> <input script> [options]
 *   -n N        logic frames to run (default: as many as the input gives)
 *   -L          the input script counts logic frames; by default it counts
 *               video frames and is converted by running the timed machine
 *               once and recording the pad latched for each logic frame
 *   -N          no hooks in B (self-check: A and B must match 100%)
 *   -o NAME     enable only this hook (repeatable)
 *   -x NAME     disable this hook (repeatable)
 *   -c          shadow-check every hooked call (interpreter on a copy)
 *
 * The dead stack ($0100 up to SP) is not compared: native code does not
 * leave the return addresses of its internal calls there. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hook.h"

#define WRAM 0x20000

static Snes rec, ma, mb;

static int at_exit(const Snes *s)
{
    uint32_t pc = (uint32_t)s->cpu.pb << 16 | s->cpu.pc;
    return s->wait_at[pc & 0xFFFF] && snes_wait_point(s, pc) == WAIT_EXIT;
}

static int run_frame(Snes *s)
{
    long n = 0;
    do {
        snes_step(s);
        if (++n > 50000000) return -1;
    } while (!at_exit(s));
    return 0;
}

/* Pad latched for each logic frame of a timed run with video-frame input. */
static uint16_t *record_input(const char *rom, const uint16_t *video, uint32_t vlen,
                              long frames, uint32_t *len)
{
    if (snes_load(&rec, rom)) return NULL;
    snes_power(&rec);
    rec.input = video;
    rec.input_len = vlen;
    uint16_t *in = calloc(frames, 2);
    long n = 0;
    while (n < frames && rec.frame < vlen + 60) {
        if (run_frame(&rec)) break;
        in[n++] = rec.joy[0];
    }
    *len = n;
    snes_free(&rec);
    return in;
}

static int compare(long frame)
{
    Cpu *a = &ma.cpu, *b = &mb.cpu;
    int bad = 0;
#define REG(r) if (a->r != b->r) { printf("  %s: A %X  B %X\n", #r, a->r, b->r); bad = 1; }
    REG(a) REG(x) REG(y) REG(p) REG(sp) REG(d) REG(db) REG(pb) REG(pc) REG(e)
#undef REG
    uint32_t lo[16], hi[16];
    int nr = 0, nbytes = 0;
    for (uint32_t i = 0; i < WRAM; i++) {
        if (i >= 0x100 && i <= a->sp) continue;
        if (ma.wram[i] == mb.wram[i]) continue;
        nbytes++;
        if (nr && i - hi[nr - 1] < 16) hi[nr - 1] = i;
        else if (nr < 16) { lo[nr] = hi[nr] = i; nr++; }
    }
    if (nbytes) {
        bad = 1;
        printf("  %d WRAM bytes differ:\n", nbytes);
        for (int r = 0; r < nr; r++) {
            printf("    $%06X-$%06X  A:", 0x7E0000 + lo[r], 0x7E0000 + hi[r]);
            for (uint32_t i = lo[r]; i <= hi[r] && i < lo[r] + 16; i++) printf(" %02X", ma.wram[i]);
            printf("  B:");
            for (uint32_t i = lo[r]; i <= hi[r] && i < lo[r] + 16; i++) printf(" %02X", mb.wram[i]);
            printf("\n");
        }
    }
    for (int r = 0; r < 0x40; r++)
        if (ma.ppu[r] != mb.ppu[r]) { printf("  PPU $21%02X: A %02X  B %02X\n", r, ma.ppu[r], mb.ppu[r]); bad = 1; }
    if (memcmp(ma.dma, mb.dma, sizeof ma.dma)) { printf("  DMA registers differ\n"); bad = 1; }
    if (ma.wram_addr != mb.wram_addr) { printf("  $2181 address differs\n"); bad = 1; }
    if (bad) printf("frame %ld: A and B differ (last hook run in B: %s)\n", frame,
                    g_last_hook >= 0 ? g_hooks[g_last_hook].name : "none");
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: hybrid <rom> <input script> [-n N] [-L] [-N] [-c] "
                "[-o NAME]... [-x NAME]...\n");
        return 2;
    }
    long frames = -1;
    int logic_input = 0, hooks = 1;
    uint8_t enable[64];
    int only = 0;
    memset(enable, 1, sizeof enable);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-L")) logic_input = 1;
        else if (!strcmp(argv[i], "-N")) hooks = 0;
        else if (!strcmp(argv[i], "-c")) g_hook_check = 1;
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "-x")) && i + 1 < argc) {
            int k = hook_find(argv[i + 1]);
            if (k < 0) { fprintf(stderr, "no hook %s\n", argv[i + 1]); return 2; }
            if (argv[i][1] == 'o') {
                if (!only) memset(enable, 0, sizeof enable);
                only = 1;
                enable[k] = 1;
            } else enable[k] = 0;
            i++;
        } else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    }

    uint32_t vlen, len;
    uint16_t *video = snes_load_input(argv[2], &vlen);
    if (!video) { fprintf(stderr, "can't read %s\n", argv[2]); return 2; }
    uint16_t *input = video;
    len = vlen;
    if (!logic_input) {
        input = record_input(argv[1], video, vlen, frames > 0 ? frames : 100000, &len);
        if (!input) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
        printf("recorded %u logic frames of input\n", len);
    }
    if (frames < 0 || frames > (long)len) frames = len;

    if (snes_load(&ma, argv[1]) || snes_load(&mb, argv[1])) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
    Snes *m[2] = {&ma, &mb};
    for (int k = 0; k < 2; k++) {
        snes_power(m[k]);
        m[k]->framestep = 1;
        m[k]->input = input;
        m[k]->input_len = len;
    }
    if (hooks) hook_install(&mb, enable);

    long n;
    int bad = 0;
    for (n = 0; n < frames; n++) {
        g_last_hook = -1;
        if (run_frame(&ma) || run_frame(&mb)) {
            printf("frame %ld: no wait-loop exit (A at %02X:%04X, B at %02X:%04X)\n", n,
                   ma.cpu.pb, ma.cpu.pc, mb.cpu.pb, mb.cpu.pc);
            printf("  A: nmitimen %02X raised %d x %04X  B: nmitimen %02X raised %d\n",
                   ma.nmitimen, ma.fs_raised, ma.cpu.x, mb.nmitimen, mb.fs_raised);
            bad = 1;
            break;
        }
        if (g_hook_check_failed) { printf("frame %ld: shadow check failed\n", n); bad = 1; break; }
        if (compare(n)) { bad = 1; break; }
    }
    if (!bad) printf("OK: %ld logic frames match\n", n);
    if (hooks) {
        printf("hook calls (fallbacks):\n");
        for (int i = 0; i < g_nhooks; i++)
            printf("  %-18s %06X %9ld (%ld)%s\n", g_hooks[i].name, g_hooks[i].addr,
                   g_hook_calls[i], g_hook_fallbacks[i], enable[i] ? "" : "  [off]");
    }
    snes_free(&ma);
    snes_free(&mb);
    return bad;
}
