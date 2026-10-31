/* Runs the ROM on the port's interpreter with an input script and compares
 * WRAM at every logic-frame start with a Mesen trace from port/trace.lua.
 *
 * Usage: verify <rom> <input script> <trace.bin> [options]
 *   -n N        stop after N logic frames
 *   -k          keep going after a mismatch (prints one line per bad frame)
 *   -p N        on mismatch, print the last N instructions (default 0)
 *   -w ADDR     log writes to WRAM address ADDR (hex, $7E/$7F or $00 bank)
 *   -i LO-HI    ignore WRAM range (hex offsets into the 128 KiB), repeatable
 *   -x PC       print the master clock each time PC (hex, 24-bit) executes
 *   -T N        print every instruction of logic frame N with its clock
 *   -t          print each frame's clock offset from Mesen (timing drift)
 *   -s          also compare the dead stack (the $100 bytes below SP), which
 *               holds interrupt frames whose PCs depend on exact timing
 *
 * A logic frame starts when the CPU leaves one of the game's NMI wait loops
 * (lda $0036 / cmp $0036 / beq), at the instruction after the beq; the loops
 * are found by scanning the ROM for that idiom (CC:004C is the main one). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes.h"

#define WRAM 0x20000
#define MAXRANGES 32

static Snes snes;
static uint8_t ref[WRAM];
static uint8_t mask[WRAM];          /* 1 = ignored */

static struct { uint32_t pc; uint8_t p, e; } hist[4096];
static unsigned hist_n, hist_max;

static long watch = -1;
static void (*real_write)(void *, uint32_t, uint8_t);
static uint32_t cur_pc;

static void watch_write(void *ctx, uint32_t addr, uint8_t v)
{
    uint8_t b = addr >> 16;
    uint16_t a = addr;
    long w = (b & 0xFE) == 0x7E ? (long)(addr & 0x1FFFF)
           : (!(b & 0x40) && a < 0x2000) ? a : -1;
    if (w == watch)
        printf("  write %05lX = %02X  pc %06X  frame %u line %u\n", w, v, cur_pc,
               snes.frame, snes.vcount);
    real_write(ctx, addr, v);
}

/* Input script: "<frame> <buttons>" lines, held until the next line. */
static uint16_t *load_input(const char *path, uint32_t *len)
{
    static const struct { const char *name; uint16_t bit; } names[] = {
        {"a", PAD_A}, {"b", PAD_B}, {"x", PAD_X}, {"y", PAD_Y}, {"l", PAD_L},
        {"r", PAD_R}, {"start", PAD_START}, {"select", PAD_SELECT},
        {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
    };
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    uint32_t cap = 1024, n = 0, frame;
    uint16_t *in = calloc(cap, 2), held = 0;
    char line[256], btn[200];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%u %199s", &frame, btn) != 2) continue;
        while (n < frame) {
            if (n == cap) { cap *= 2; in = realloc(in, cap * 2); }
            in[n++] = held;
        }
        if (!strcmp(btn, "end")) break;
        held = 0;
        for (char *t = strtok(btn, "+"); t; t = strtok(NULL, "+"))
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (!strcmp(t, names[i].name)) held |= names[i].bit;
    }
    fclose(f);
    *len = n;
    return in;
}

/* Exit addresses of the NMI wait loops, as a bitmap over the low 16 bits
 * plus the exact list. */
static uint32_t exits[64];
static int nexits;
static uint8_t exit_lo[0x10000];

static void find_wait_loops(void)
{
    static const uint8_t pat[8] = {0xAD, 0x36, 0x00, 0xCD, 0x36, 0x00, 0xF0, 0xFB};
    for (uint32_t o = 0; o + 8 <= snes.rom_size && nexits < 62; o++) {
        if (memcmp(snes.rom + o, pat, 8)) continue;
        uint32_t x = o + 8;
        exits[nexits++] = 0xC00000 + x;
        if ((x & 0xFFFF) >= 0x8000 && x < 0x400000) exits[nexits++] = x;
        exit_lo[x & 0xFFFF] = 1;
    }
}

static int at_exit(uint32_t pc)
{
    if (!exit_lo[pc & 0xFFFF]) return 0;
    for (int i = 0; i < nexits; i++) if (exits[i] == pc) return 1;
    return 0;
}

static void print_history(unsigned n)
{
    if (n > hist_n) n = hist_n;
    if (n > hist_max) n = hist_max;
    printf("last %u instructions:\n", n);
    for (unsigned i = hist_n - n; i < hist_n; i++) {
        char buf[64];
        unsigned k = i % hist_max;
        cpu_disasm(buf, hist[k].pc, hist[k].e || (hist[k].p & FM),
                   hist[k].e || (hist[k].p & FX), snes_peek, &snes);
        printf("  %02X:%04X  %s\n", hist[k].pc >> 16, hist[k].pc & 0xFFFF, buf);
    }
}

/* Prints differing ranges (merging gaps under 16 bytes); returns the number
 * of differing bytes. */
static int dead_stack = 1;

static int report_diff(int verbose)
{
    int total = 0, nr = 0;
    uint32_t lo[MAXRANGES], hi[MAXRANGES];
    uint32_t sp = snes.cpu.sp, sp_lo = dead_stack ? (sp > 0xFF ? sp - 0xFF : 0) : 1;
    for (uint32_t a = 0; a < WRAM; a++) {
        if (mask[a] || snes.wram[a] == ref[a]) continue;
        if (a >= sp_lo && a <= sp) continue;
        total++;
        if (nr && a - hi[nr - 1] < 16) hi[nr - 1] = a;
        else if (nr < MAXRANGES) { lo[nr] = hi[nr] = a; nr++; }
    }
    if (!verbose) return total;
    for (int r = 0; r < nr; r++) {
        printf("  $%06X-$%06X\n", 0x7E0000 + lo[r], 0x7E0000 + hi[r]);
        for (uint32_t a = lo[r] & ~15u; a <= hi[r] && a < (lo[r] & ~15u) + 64; a += 16) {
            printf("    %06X mesen:", 0x7E0000 + a);
            for (int i = 0; i < 16; i++) printf(" %02X", ref[a + i]);
            printf("\n           port: ");
            for (int i = 0; i < 16; i++)
                printf(snes.wram[a + i] != ref[a + i] ? "%02X " : "-- ", snes.wram[a + i]);
            printf("\n");
        }
    }
    if (nr == MAXRANGES) printf("  (more ranges not shown)\n");
    return total;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: verify <rom> <input script> <trace.bin> "
                "[-n N] [-k] [-s] [-t] [-x PC] [-T N] [-p N] [-w ADDR] [-i LO-HI]...\n");
        return 2;
    }
    long maxframes = -1;
    int keep = 0, timing = 0;
    long xpc = -1, tframe = -1;
    unsigned pcs = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) maxframes = atol(argv[++i]);
        else if (!strcmp(argv[i], "-k")) keep = 1;
        else if (!strcmp(argv[i], "-s")) dead_stack = 0;
        else if (!strcmp(argv[i], "-t")) timing = 1;
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) tframe = atol(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) xpc = strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) pcs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) watch = strtol(argv[++i], NULL, 16) & 0x1FFFF;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            unsigned lo, hi;
            if (sscanf(argv[++i], "%x-%x", &lo, &hi) == 2)
                for (unsigned a = lo; a <= hi && a < WRAM; a++) mask[a] = 1;
        } else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    }
    hist_max = pcs > 0 ? (pcs < 4096 ? pcs : 4096) : 1;

    if (snes_load(&snes, argv[1])) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
    uint32_t input_len;
    uint16_t *input = load_input(argv[2], &input_len);
    if (!input) { fprintf(stderr, "can't read %s\n", argv[2]); return 2; }
    FILE *tf = fopen(argv[3], "rb");
    if (!tf) { fprintf(stderr, "can't read %s\n", argv[3]); return 2; }
    char metapath[1024];
    snprintf(metapath, sizeof metapath, "%s.txt", argv[3]);
    FILE *mf = fopen(metapath, "r");

    find_wait_loops();
    snes_power(&snes);
    snes.input = input;
    snes.input_len = input_len;
    if (watch >= 0) { real_write = snes.cpu.write; snes.cpu.write = watch_write; }

    long n = 0, bad = 0, first_bad = -1;
    int warned_timing = 0;
    uint64_t budget = 0;
    while (maxframes < 0 || n < maxframes) {
        Cpu *c = &snes.cpu;
        uint32_t pc = (uint32_t)c->pb << 16 | c->pc;
        if (at_exit(pc)) {
            if (fread(ref, 1, WRAM, tf) != WRAM) break;
            unsigned long rn = 0, rframe = 0, rexit = 0, rline = 0, rlate = 0;
            unsigned long long rclock = 0;
            int have_meta = mf && fscanf(mf, "%lu %lu %lx %llu %lu %lu", &rn, &rframe,
                                         &rexit, &rclock, &rline, &rlate) == 6;
            if (have_meta && !warned_timing && (rexit != pc || rframe != snes.frame)) {
                printf("frame %ld: timing differs: mesen exit %06lX video frame %lu, "
                       "port exit %06X video frame %u (clock %+lld)\n", n, rexit, rframe,
                       pc, snes.frame, (long long)snes.clock - (long long)rclock);
                warned_timing = 1;
            }
            if (have_meta && timing)
                printf("frame %ld: video %u/%lu clock %+lld line %u/%lu\n", n, snes.frame, rframe,
                       (long long)snes.clock - (long long)rclock, snes.vcount, rline);
            int d = report_diff(0);
            if (d) {
                bad++;
                if (first_bad < 0) {
                    first_bad = n;
                    printf("frame %ld (exit %06X, video frame %u): %d bytes differ\n",
                           n, pc, snes.frame, d);
                    report_diff(1);
                    if (pcs) print_history(pcs);
                    if (!keep) break;
                } else {
                    printf("frame %ld: %d bytes differ\n", n, d);
                }
            }
            n++;
            budget = 0;
        }
        cur_pc = pc;
        if (n == tframe)
            printf("T %06X %llu\n", pc, (unsigned long long)snes.clock);
        if (pc == xpc)
            printf("  pc %06X clock %llu line %u frame %ld\n", pc,
                   (unsigned long long)snes.clock, snes.vcount, n);
        if (pcs) {
            unsigned k = hist_n++ % hist_max;
            hist[k].pc = pc; hist[k].p = c->p; hist[k].e = c->e;
        }
        snes_step(&snes);
        if (++budget > 50000000) {
            printf("frame %ld: no frame boundary in 50M instructions (pc %06X)\n", n, pc);
            if (pcs) print_history(pcs);
            break;
        }
    }
    if (first_bad < 0)
        printf("OK: %ld logic frames match\n", n);
    else
        printf("%ld logic frames compared, %ld differ, first at %ld (%ld matched before it)\n",
               n, bad, first_bad, first_bad);
    fclose(tf);
    if (mf) fclose(mf);
    free(input);
    snes_free(&snes);
    return first_bad < 0 ? 0 : 1;
}
