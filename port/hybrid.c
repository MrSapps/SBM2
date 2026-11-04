/* Twin verifier for the hybrid port: two machines in frame-step mode from
 * power-on with the same input, A pure interpreter, B with the native C
 * routines (hooks) enabled. After every logic frame (wait-loop exit) their
 * WRAM, CPU registers, PPU and DMA registers must be identical.
 *
 * Usage: hybrid <rom> <input> [options]
 * <input> is a pad log (*.inputs, from bot/inputlog.lua: every pad read and
 * NMI of a real run since power-on), or an input script in video frames
 * (bot/harness.lua format), turned into a pad log by running the timed
 * machine once.
 *   -n N        logic frames to run (default: until the log is used up)
 *   -L          the input script counts logic frames (pad latched per frame)
 *   -W FILE     write the pad-read log used
 *   -R TRACE    also compare A with a Mesen WRAM trace (port/trace.lua) at
 *               every logic frame it has (TRACE.txt numbers them; a trace
 *               from a savestate starts later); summarises
 *   -i LO-HI    ignore this WRAM range (hex offsets) in the -R comparison
 *   -N          no hooks in B (self-check: A and B must match 100%)
 *   -o NAME     enable only this hook (repeatable)
 *   -x NAME     disable this hook (repeatable)
 *   -c          shadow-check every hooked call (interpreter on a copy)
 *   -w ADDR     log A's writes to WRAM address ADDR (hex offset)
 *   -P FILE     profile: B's interpreted instructions per 24-bit PC, written
 *               as "pc count" lines (tools/sbm2/profile.py sums them up)
 *   -D N:FILE   write A's WRAM (128 KiB) after logic frame N, or after the
 *               last one for N = end (repeatable; bot/test reads these)
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
static long watch = -1, cur_frame;
static void (*real_write)(void *, uint32_t, uint8_t);
#define MAX_DUMPS 64
static long dump_at[MAX_DUMPS];
static const char *dump_path[MAX_DUMPS];
static int ndumps;

static void dump_wram(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(ma.wram, 1, WRAM, f) != WRAM) fprintf(stderr, "can't write %s\n", path);
    if (f) fclose(f);
}

static void watch_write(void *ctx, uint32_t addr, uint8_t v)
{
    uint8_t b = addr >> 16;
    long w = (b & 0xFE) == 0x7E ? (long)(addr & 0x1FFFF)
           : (!(b & 0x40) && (addr & 0xFFFF) < 0x2000) ? (long)(addr & 0xFFFF) : -1;
    if (w == watch)
        printf("  write %05lX = %02X  pc %02X:%04X  frame %ld\n", w, v, ma.cpu.pb, ma.cpu.pc, cur_frame);
    real_write(ctx, addr, v);
}

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

/* Pad reads of a timed run with video-frame input, for up to `frames`
 * logic frames. */
static uint16_t *record_pads(const char *rom, const uint16_t *video, uint32_t vlen,
                             long frames, uint32_t *len)
{
    if (snes_load(&rec, rom)) return NULL;
    snes_power(&rec);
    rec.input = video;
    rec.input_len = vlen;
    rec.pad_record = 1;
    long n = 0;
    while (n < frames && rec.frame < vlen + 60) {
        if (run_frame(&rec)) break;
        n++;
    }
    uint16_t *pads = malloc((rec.pad_rec_len + 1) * 2);
    memcpy(pads, rec.pad_rec, rec.pad_rec_len * 2);
    *len = rec.pad_rec_len;
    snes_free(&rec);
    return pads;
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
        if (verbose > 0 && n < 24)
            printf("    $%06X: mesen %02X  frame-step %02X\n", 0x7E0000 + i, ref[i], ma.wram[i]);
        if (verbose >= 0) ever[i]++;
        n++;
    }
    if (verbose > 0) printf("  frame %ld: %ld bytes differ from Mesen\n", frame, n);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: hybrid <rom> <input> [-n N] [-L] [-N] [-c] [-W FILE] "
                "[-R TRACE] [-i LO-HI]... [-o NAME]... [-x NAME]... [-D N:FILE]...\n");
        return 2;
    }
    long frames = -1;
    int logic_input = 0, hooks = 1;
    uint8_t enable[64];
    int only = 0;
    const char *wpath = NULL, *rpath = NULL, *ppath = NULL;
    memset(enable, 1, sizeof enable);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-L")) logic_input = 1;
        else if (!strcmp(argv[i], "-N")) hooks = 0;
        else if (!strcmp(argv[i], "-c")) g_hook_check = 1;
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) ppath = argv[++i];
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) wpath = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) watch = strtol(argv[++i], NULL, 16) & 0x1FFFF;
        else if (!strcmp(argv[i], "-R") && i + 1 < argc) rpath = argv[++i];
        else if (!strcmp(argv[i], "-D") && i + 1 < argc && ndumps < MAX_DUMPS) {
            char *arg = argv[++i], *colon = strchr(arg, ':');
            if (!colon) { fprintf(stderr, "-D wants N:FILE\n"); return 2; }
            dump_at[ndumps] = strncmp(arg, "end:", 4) ? strtol(arg, NULL, 10) : -1;
            dump_path[ndumps++] = colon + 1;
        }
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

    uint32_t vlen, len = 0;
    size_t al = strlen(argv[2]);
    int is_log = al > 7 && !strcmp(argv[2] + al - 7, ".inputs");
    uint16_t *script = is_log ? load_log(argv[2], &vlen) : snes_load_input(argv[2], &vlen);
    if (!script) { fprintf(stderr, "can't read %s\n", argv[2]); return 2; }
    uint16_t *pads = NULL;
    if (is_log) {
        pads = script;
        len = vlen;
    } else if (!logic_input) {
        pads = record_pads(argv[1], script, vlen, frames > 0 ? frames : 1000000, &len);
        if (!pads) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
        printf("recorded %u pad reads\n", len);
    }
    if (frames < 0) frames = pads ? 100000000 : (long)vlen;
    if (wpath && pads) {
        FILE *f = fopen(wpath, "wb");
        for (uint32_t i = 0; f && i < len; i++) { fputc(pads[i] & 0xFF, f); fputc(pads[i] >> 8, f); }
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
    int markers = 0;
    for (uint32_t i = 0; pads && i < len; i++) if (pads[i] & 0xF) { markers = 1; break; }

    if (snes_load(&ma, argv[1]) || snes_load(&mb, argv[1])) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
    Snes *m[2] = {&ma, &mb};
    for (int k = 0; k < 2; k++) {
        snes_power(m[k]);
        m[k]->framestep = 1;
        if (pads) { m[k]->pads = pads; m[k]->npads = len; m[k]->pad_markers = markers; }
        else { m[k]->input = script; m[k]->input_len = vlen; }
    }
    if (hooks) hook_install(&mb, enable);
    if (ppath) mb.prof = calloc(1 << 24, 4);
    if (watch >= 0) { real_write = ma.cpu.write; ma.cpu.write = watch_write; }

    long n;
    int bad = 0;
    for (n = 0; n < frames && !(pads && ma.pad_pos >= len); n++) {
        g_last_hook = -1;
        cur_frame = n;
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
        for (int k = 0; k < ndumps; k++) if (dump_at[k] == n) dump_wram(dump_path[k]);
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
    for (int k = 0; k < ndumps; k++) if (dump_at[k] < 0) dump_wram(dump_path[k]);
    if (!bad) printf("OK: %ld logic frames match\n", n);
    if (pads) printf("pad log: %u of %u entries used, %u did not fit\n", ma.pad_pos, len, ma.pad_desync);
    if (hooks) {
        printf("hook calls (fallbacks):\n");
        for (int i = 0; i < g_nhooks; i++)
            printf("  %-18s %06X %9ld (%ld)%s\n", g_hooks[i].name, g_hooks[i].addr,
                   g_hook_calls[i], g_hook_fallbacks[i], enable[i] ? "" : "  [off]");
    }
    if (ppath) {
        FILE *f = fopen(ppath, "w");
        for (uint32_t pc = 0; f && pc < 1 << 24; pc++) if (mb.prof[pc]) fprintf(f, "%06X %u\n", pc, mb.prof[pc]);
        if (f) fclose(f);
    }
    snes_free(&ma);
    snes_free(&mb);
    return bad;
}
