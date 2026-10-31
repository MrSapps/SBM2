/* Twin verifier for the hybrid port: two machines in frame-step mode from
 * power-on with the same input, A pure interpreter, B with the native C
 * routines (hooks) enabled. After every logic frame (wait-loop exit) their
 * WRAM, CPU registers, PPU and DMA registers must be identical.
 *
 * Usage: hybrid <rom> <input> [options]
 * <input> is a logic-frame input log (*.inputs, from bot/inputlog.lua: one
 * little-endian pad word per logic frame since power-on), or an input script
 * in video frames (bot/harness.lua format), converted by running the timed
 * machine once and recording the pad latched for each logic frame.
 *   -n N        logic frames to run (default: all of the input)
 *   -L          the input script counts logic frames instead
 *   -W FILE     write the logic-frame input used as an .inputs log
 *   -R TRACE    also compare A with a Mesen WRAM trace (port/trace.lua) at
 *               every logic frame it has (it may start later, e.g. from a
 *               savestate: TRACE.txt gives the frame numbers); summarises
 *   -i LO-HI    ignore this WRAM range (hex offsets) in the -R comparison
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
static uint8_t ref[WRAM], ignore[WRAM];
static long ever[WRAM];

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

static uint16_t *load_log(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(size + 1);
    uint16_t *in = calloc(size / 2 + 1, 2);
    if (fread(b, 1, size, f) != (size_t)size) size = 0;
    fclose(f);
    for (long i = 0; i + 1 < size; i += 2) in[i / 2] = b[i] | b[i + 1] << 8;
    free(b);
    *len = size / 2;
    return in;
}

/* A vs the Mesen trace: returns the number of differing bytes. */
static long compare_ref(long frame, int verbose)
{
    long n = 0;
    for (uint32_t i = 0; i < WRAM; i++) {
        if (ignore[i] || (i >= 0x100 && i <= ma.cpu.sp) || ma.wram[i] == ref[i]) continue;
        if (verbose && n < 24)
            printf("    $%06X: mesen %02X  frame-step %02X\n", 0x7E0000 + i, ref[i], ma.wram[i]);
        ever[i]++;
        n++;
    }
    if (verbose) printf("  frame %ld: %ld bytes differ from Mesen\n", frame, n);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: hybrid <rom> <input> [-n N] [-L] [-N] [-c] [-W FILE] "
                "[-R TRACE] [-i LO-HI]... [-o NAME]... [-x NAME]...\n");
        return 2;
    }
    long frames = -1;
    int logic_input = 0, hooks = 1;
    uint8_t enable[64];
    int only = 0;
    const char *wpath = NULL, *rpath = NULL;
    memset(enable, 1, sizeof enable);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-L")) logic_input = 1;
        else if (!strcmp(argv[i], "-N")) hooks = 0;
        else if (!strcmp(argv[i], "-c")) g_hook_check = 1;
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) wpath = argv[++i];
        else if (!strcmp(argv[i], "-R") && i + 1 < argc) rpath = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            unsigned lo, hi;
            if (sscanf(argv[++i], "%x-%x", &lo, &hi) == 2)
                for (unsigned a = lo; a <= hi && a < WRAM; a++) ignore[a] = 1;
        }
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
    size_t al = strlen(argv[2]);
    int is_log = al > 7 && !strcmp(argv[2] + al - 7, ".inputs");
    uint16_t *video = is_log ? load_log(argv[2], &vlen) : snes_load_input(argv[2], &vlen);
    if (!video) { fprintf(stderr, "can't read %s\n", argv[2]); return 2; }
    uint16_t *input = video;
    len = vlen;
    if (!logic_input && !is_log) {
        input = record_input(argv[1], video, vlen, frames > 0 ? frames : 100000, &len);
        if (!input) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
        printf("recorded %u logic frames of input\n", len);
    }
    if (frames < 0 || frames > (long)len) frames = len;
    if (wpath) {
        FILE *f = fopen(wpath, "wb");
        for (uint32_t i = 0; f && i < len; i++) { fputc(input[i] & 0xFF, f); fputc(input[i] >> 8, f); }
        if (f) fclose(f);
    }
    FILE *rf = rpath ? fopen(rpath, "rb") : NULL;
    if (rpath && !rf) { fprintf(stderr, "can't read %s\n", rpath); return 2; }
    long ref_first = -1, ref_frames = 0, ref_clean = 0, ref_start = 0;
    if (rf) {
        char mp[1024];
        snprintf(mp, sizeof mp, "%s.txt", rpath);
        FILE *mf = fopen(mp, "r");
        if (mf && fscanf(mf, "%ld", &ref_start) != 1) ref_start = 0;
        if (mf) fclose(mf);
    }

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
        if (rf && n >= ref_start && fread(ref, 1, WRAM, rf) == WRAM) {
            ref_frames++;
            long d = compare_ref(n, 0);
            if (!d) ref_clean++;
            else if (ref_first < 0) { ref_first = n; compare_ref(n, 1); }
        }
    }
    if (rf) {
        printf("vs Mesen: %ld of %ld frames identical, first difference at frame %ld\n",
               ref_clean, ref_frames, ref_first);
        printf("  addresses that differed (frames):");
        int k = 0;
        for (uint32_t i = 0; i < WRAM && k < 40; i++)
            if (ever[i]) { printf(" %06X(%ld)", 0x7E0000 + i, ever[i]); k++; }
        printf("\n");
        fclose(rf);
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
