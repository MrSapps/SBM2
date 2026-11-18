/* Checks the sound side against another player: plays an .spc snapshot
 * (build/port/play -D) through port/spc.c and through libgme (blargg's
 * SPC emulation, loaded at run time), and compares the two.
 *
 * Usage: spc_check FILE.spc [seconds] [ours.wav gme.wav]
 * Prints the correlation of the two (best over a small lag) per second
 * after the first, and fails if the median is below 0.9. The .spc format
 * keeps no voice state (envelopes, sample positions), so the first notes
 * differ; both start from the same registers and RAM. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spc.h"

typedef struct Music_Emu Music_Emu;
typedef const char *(*open_data_f)(const void *, long, Music_Emu **, int);
typedef const char *(*start_f)(Music_Emu *, int);
typedef const char *(*play_f)(Music_Emu *, int, short *);
typedef void (*ignore_f)(Music_Emu *, int);

static void wav(const char *path, const int16_t *s, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t bytes = (uint32_t)(n * 4), rate = 32000, br = rate * 4, h32;
    uint16_t h16;
    fwrite("RIFF", 1, 4, f); h32 = 36 + bytes; fwrite(&h32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); h32 = 16; fwrite(&h32, 4, 1, f);
    h16 = 1; fwrite(&h16, 2, 1, f); h16 = 2; fwrite(&h16, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    h16 = 4; fwrite(&h16, 2, 1, f); h16 = 16; fwrite(&h16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(s, 4, (size_t)n, f);
    fclose(f);
}

/* normalised correlation of a[0..n) with b[lag..lag+n), mono (L+R) */
static double corr(const int16_t *a, const int16_t *b, long n, int lag)
{
    double sab = 0, saa = 0, sbb = 0;
    for (long i = 0; i < n; i++) {
        double x = a[2 * i] + a[2 * i + 1], y = b[2 * (i + lag)] + b[2 * (i + lag) + 1];
        sab += x * y, saa += x * x, sbb += y * y;
    }
    return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : 1.0;
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: spc_check FILE.spc [seconds] [ours.wav gme.wav]\n"); return 2; }
    int secs = argc > 2 ? atoi(argv[2]) : 8;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "can't read %s\n", argv[1]); return 2; }
    static uint8_t img[0x10200];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    void *lib = dlopen("libgme.so.0", RTLD_NOW);
    if (!lib) { fprintf(stderr, "no libgme (%s): skipped\n", dlerror()); return 0; }
    open_data_f gopen = (open_data_f)dlsym(lib, "gme_open_data");
    start_f gstart = (start_f)dlsym(lib, "gme_start_track");
    play_f gplay = (play_f)dlsym(lib, "gme_play");
    ignore_f gignore = (ignore_f)dlsym(lib, "gme_ignore_silence");
    if (!gopen || !gstart || !gplay) { fprintf(stderr, "libgme: missing symbols\n"); return 2; }

    long ns = 32000L * secs;
    int16_t *g = calloc((size_t)ns * 2, 2);
    Music_Emu *emu;
    const char *err = gopen(img, (long)n, &emu, 32000);
    if (err) { fprintf(stderr, "gme: %s\n", err); return 2; }
    if (gignore) gignore(emu, 1);
    if ((err = gstart(emu, 0))) { fprintf(stderr, "gme: %s\n", err); return 2; }
    for (long i = 0; i < ns; i += 1024) gplay(emu, (int)(2 * (ns - i < 1024 ? ns - i : 1024)), g + 2 * i);

    static Spc s;
    if (spc_load(&s, img, n)) { fprintf(stderr, "not an .spc\n"); return 2; }
    while ((long)s.nsamples < 2 * ns) spc_step(&s);
    const int16_t *o = s.samples;
    if (argc > 4) wav(argv[3], o, ns), wav(argv[4], g, ns);

    const int maxlag = 64;
    double c[64];
    int nc = 0;
    for (int sec = 1; sec + 1 < secs && nc < 64; sec++) {
        long a = 32000L * sec;
        double best = -1;
        int bl = 0;
        for (int lag = -maxlag; lag <= maxlag; lag++) {
            double v = corr(o + 2 * a, g + 2 * a, 32000, lag);
            if (v > best) best = v, bl = lag;
        }
        printf("second %d: correlation %.4f (lag %d)\n", sec, best, bl);
        c[nc++] = best;
    }
    qsort(c, (size_t)nc, sizeof *c, cmpd);
    double med = nc ? c[nc / 2] : 0;
    printf("median %.4f\n", med);
    return med < 0.9;
}
