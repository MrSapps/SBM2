/* Front-end for the hybrid port: the interpreter plus the native C routines
 * (hooks), in timed mode, with the PPU drawing every line.
 *
 * Usage: play <rom> [options]
 *   -i FILE     input script ("<frame> <buttons>" lines, bot/harness.lua);
 *               without it, and with SDL, the keyboard drives pad 1
 *   -n N        stop after N video frames
 *   -s LIST     headless: write frames LIST (comma-separated) as PPM files
 *   -o DIR      where -s writes (default .)
 *   -N          no hooks (pure interpreter)
 *   -x NAME     disable this hook (repeatable)
 *
 * Keys: arrows, Z = B, X = A, A = Y, S = X, Q = L, W = R, Enter = Start,
 * Right Shift = Select, Esc quits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hook.h"
#include "ppu.h"
#ifdef HAVE_SDL
#include <SDL2/SDL.h>
#endif

static Snes m;
static Ppu ppu;

static void write_ppm(const char *dir, uint32_t frame)
{
    char path[512];
    snprintf(path, sizeof path, "%s/f%06u.ppm", dir, frame);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n256 224\n255\n");
    for (int i = 0; i < 256 * 224; i++) {
        uint32_t c = ppu.pixels[i];
        uint8_t px[3] = {c >> 16, c >> 8, c};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

/* Runs until the next vblank starts. */
static int run_frame(void)
{
    uint32_t f = m.frame;
    long n = 0;
    while (m.frame == f) {
        snes_step(&m);
        if (++n > 50000000) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: play <rom> [-i input] [-n frames] [-s f1,f2 -o dir] [-N] [-x hook]\n"); return 2; }
    const char *input = NULL, *outdir = ".", *shots = NULL;
    long frames = -1;
    int hooks = 1;
    uint8_t *enable = NULL;
    if (snes_load(&m, argv[1])) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-N")) hooks = 0;
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) {
            int h = hook_find(argv[++i]);
            if (h < 0) { fprintf(stderr, "no hook %s\n", argv[i]); return 2; }
            if (!enable) { enable = malloc(g_nhooks); memset(enable, 1, g_nhooks); }
            enable[h] = 0;
        } else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    }
    /* Pad 1 per video frame; the keyboard fills it in as we go. */
    uint32_t cap = 1 << 22, len = 0;
    uint16_t *pads = calloc(cap, sizeof *pads);
    if (input) {
        uint16_t *script = snes_load_input(input, &len);
        if (!script) { fprintf(stderr, "can't read %s\n", input); return 2; }
        memcpy(pads, script, (len < cap ? len : cap) * sizeof *pads);
        free(script);
    }
    snes_power(&m);
    m.ppu_state = &ppu;
    m.input = pads;
    m.input_len = cap;
    if (hooks) hook_install(&m, enable);

    int headless = shots != NULL;
#ifndef HAVE_SDL
    headless = 1;
#endif
    if (headless) {
        long last = frames;
        if (shots && last < 0) {
            for (const char *p = shots; *p; ) { long f = strtol(p, (char **)&p, 10); if (f > last) last = f; if (*p == ',') p++; }
        }
        if (last < 0) last = len;
        for (long f = 1; f <= last; f++) {
            if (run_frame()) { fprintf(stderr, "stuck at frame %ld\n", f); return 1; }
            for (const char *p = shots ? shots : ""; *p; ) {
                if (strtol(p, (char **)&p, 10) == f) write_ppm(outdir, m.frame);
                if (*p == ',') p++;
            }
        }
        return 0;
    }
#ifdef HAVE_SDL
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("SBM2 port", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       256 * 3, 224 * 3, SDL_WINDOW_RESIZABLE);
    /* Paced by the clock below, not vsync (a vsync present can block forever
     * on some X setups). */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, 256, 224);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 224);
    static const struct { SDL_Scancode k; uint16_t b; } keys[] = {
        {SDL_SCANCODE_UP, PAD_UP}, {SDL_SCANCODE_DOWN, PAD_DOWN}, {SDL_SCANCODE_LEFT, PAD_LEFT},
        {SDL_SCANCODE_RIGHT, PAD_RIGHT}, {SDL_SCANCODE_Z, PAD_B}, {SDL_SCANCODE_X, PAD_A},
        {SDL_SCANCODE_A, PAD_Y}, {SDL_SCANCODE_S, PAD_X}, {SDL_SCANCODE_Q, PAD_L},
        {SDL_SCANCODE_W, PAD_R}, {SDL_SCANCODE_RETURN, PAD_START}, {SDL_SCANCODE_RSHIFT, PAD_SELECT},
    };
    for (long f = 1; frames < 0 || f <= frames; f++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) return 0;
        const uint8_t *ks = SDL_GetKeyboardState(NULL);
        if (ks[SDL_SCANCODE_ESCAPE]) break;
        uint16_t pad = 0;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) if (ks[keys[i].k]) pad |= keys[i].b;
        /* A hook that runs a whole frame driver can pass several vblanks in
         * one step: hold the pad for the next few. */
        if (!input) for (uint32_t k = 1; k <= 8 && m.frame + k < cap; k++) pads[m.frame + k] = pad;
        if (run_frame()) { fprintf(stderr, "stuck at frame %ld\n", f); return 1; }
        SDL_UpdateTexture(tex, NULL, ppu.pixels, 256 * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        static uint64_t next;
        uint64_t now = SDL_GetTicks64();
        next = next && next > now - 100 ? next + 1000 / 60 : now + 1000 / 60;
        if (next > now) SDL_Delay((uint32_t)(next - now));
    }
    SDL_Quit();
#endif
    return 0;
}
