/* Front-end for the hybrid port: the interpreter plus the native C routines
 * (hooks), in timed mode, with the PPU drawing every line.
 *
 * Usage: play <rom> [options]
 *   -i FILE     input script ("<frame> <buttons>" lines, bot/harness.lua);
 *               without it, and with SDL, the keyboard drives pad 1
 *   -r FILE     replay a pad log (*.inputs, e.g. build/<run>/run.inputs from
 *               bot/drive.py) in frame-step mode, as the verifier does: the
 *               bot's run plays back exactly. A frame is then a logic frame,
 *               drawn whole at its end (no HDMA effects). Runs uncapped
 *   -t FILE     with -r: the log was recorded by botrun/agentrun in timed
 *               mode from a start state whose pad log is FILE (e.g.
 *               build/lib/stage_10.inputs): that prefix replays in
 *               frame-step (not shown), the rest timed, as botrun's
 *               SBM2_REPLAY_START does
 *   -f FPS      pace at FPS video frames a second (implies -p)
 *   -p          pace a replay at 60 frames a second
 *   -u          run uncapped (the default for -r)
 *   -n N        stop after N video frames
 *   -s LIST     headless: write frames LIST (comma-separated) as PPM files
 *   -o DIR      where -s writes (default .)
 *   -O FILE     overlays (port/overlay.c) from an agent run's overlay file
 *               (agentrun with SBM2_OVERLAY=FILE): the window shows them
 *   -V OUT.mp4   with -O: write the overlaid frames to a video (ffmpeg),
 *               headless
 *   -F N / -T N  with -V: video frames N.. / ..N only (the replay still runs
 *               from the start)
 *   -k K        with -V: keep every Kth frame (K x speed)
 *   -A FILE     with -V (and -k 1): the sound of the kept frames (port/spc.h's
 *               shadow of the sound CPU), raw 16-bit stereo little-endian
 *               at 48 kHz: 800 samples a video frame, so it stays with the
 *               60 fps video (the DSP's 32 kHz, ~532.4 a frame, resampled)
 *   -D F:FILE   write the sound CPU's state at video frame F as an .spc
 *   -N          no hooks (pure interpreter; the default with -r: the
 *               hooks are not cycle-exact, so a replay with them drifts off
 *               the recorded run, e.g. a death in 1-3)
 *   -H          hooks on even with -r
 *   -x NAME     disable this hook (repeatable)
 *
 * Keys: arrows, Z = B, X = A, A = Y, S = X, Q = L, W = R, Enter = Start,
 * Right Shift = Select, Esc quits; hold Tab to run 8x faster. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hook.h"
#include "ppu.h"
#include "overlay.h"
#include "spc.h"
#ifdef HAVE_SDL
#include <SDL2/SDL.h>
#endif

static Snes m;
static Ppu ppu;

/* --- sound: the shadow SPC700 (port/spc.h) on the CPU's port accesses --- */
static Spc spc;
static int sound;
static int last_read[4] = {-1, -1, -1, -1};
static uint64_t spc_base, clk_base;

/* SPC cycles (1.024 MHz) at the machine's master clock (21.477 MHz). */
static uint64_t spc_time(const Snes *s)
{
    return spc_base + (s->clock - clk_base) * 1024000ULL / 21477273ULL;
}

/* One frame's DSP samples (n stereo) to exactly 800 at 48 kHz, linear,
 * continuing from the last frame's last sample. */
static void resample_frame(const int16_t *in, size_t n, int16_t *out)
{
    static int16_t last[2];
    for (int k = 0; k < 800; k++) {
        double x = (k + 1) * (double)n / 800 - 1;   /* in input samples; -1 = last frame's */
        long i = (long)floor(x);
        double t = x - i;
        for (int c = 0; c < 2; c++) {
            double a = i < 0 ? last[c] : in[2 * i + c];
            double b = i + 1 < (long)n ? in[2 * (i + 1) + c] : a;
            out[2 * k + c] = (int16_t)lrint(a + (b - a) * t);
        }
    }
    if (n) last[0] = in[2 * (n - 1)], last[1] = in[2 * (n - 1) + 1];
}

static void apu_tap(Snes *s, int port, uint8_t v, int write)
{
    if (s != &m) return;  /* (not the futures' copies) */
    if (write) spc_feed_write(&spc, spc_time(s), port, v);
    else if (v != last_read[port]) {
        /* the CPU saw the port change: a handshake the shadow must meet */
        last_read[port] = v;
        spc_feed_sync(&spc, spc_time(s), port, v);
    }
}

/* --- futures windows (-O): oracle runs replayed on copies of the machine --- */
typedef struct {
    Snes s;
    Ppu p;
    uint16_t *buf;
    int n, len, tend, age, ended, ev;
    uint32_t f0;
} Future;
static Future fut[OV_MAXWIN];
static int nfut;

/* The point a decision's runs start from: agentrun's frame callback runs at
 * the first instruction boundary after the frame start with no NMI being
 * taken (botrun.c step_hook); the oracle copies the machine there. */
static void to_callback_point(Snes *s)
{
    for (long g = 0; g < 100000; g++) {
        if (!(s->nmi_pending && s->nmi_at <= s->cycle_start) && !s->cpu.nmi && !s->cpu.wai && !s->cpu.stp) return;
        snes_step(s);
    }
}

static void future_start(Future *f, int ev, int tend)
{
    int n;
    const uint16_t *pads = ov_pads(ev, &n);
    f->s = m;
    f->p = ppu;
    f->s.cpu.ctx = &f->s;
    f->s.ppu_state = &f->p;
    f->s.hook = NULL;
    f->s.on_vblank = NULL;
    f->s.on_frame = NULL;
    f->s.pads = NULL;
    f->s.npads = f->s.pad_pos = 0;
    f->s.pad_markers = 0;
    f->s.pad_record = 0;
    f->s.prof = NULL;
    int len = ov_len(ev), cap = (len > n ? len : n) + 3;
    f->buf = realloc(f->buf, (size_t)cap * 2);
    memset(f->buf, 0, (size_t)cap * 2);   /* (past its pads: none, e.g. a death's last frames) */
    memcpy(f->buf, pads, (size_t)n * 2);
    /* as the oracle: buf[i] is the pad of frame f0 + i, this frame's latched now */
    f->f0 = m.frame;
    f->s.input = f->buf - f->f0;
    f->s.input_len = f->f0 + (uint32_t)cap - 3;
    if (f->s.nmitimen & 0x01) f->s.joy[0] = f->buf[0];
    f->n = cap - 3;
    f->len = len;
    f->tend = tend;
    f->age = 0;
    f->ended = 0;
    f->ev = ev;
}

/* t video frames since the start: its run sped up or slowed so its
 * len frames take tend (ov_pick's timing). */
static void future_step(Future *f, uint32_t t)
{
    long want = ((long)t * f->len + f->tend - 1) / f->tend;
    if (want > f->len) want = f->len;
    while (f->age < want) {
        uint32_t fr = f->s.frame;
        for (long g = 0; f->s.frame == fr && g < 5000000; g++) snes_step(&f->s);
        f->age++;
    }
    if (f->age >= f->len) f->ended = 1;
}

/* After a frame of the replay: the overlay's events for it, new futures
 * if a decision's runs should show, each future a frame on, then draw. */
static void overlay_frame(uint32_t *canvas)
{
    int evs[OV_MAXWIN], tend[OV_MAXWIN], k;
    if (!canvas) {   /* before the part shown: the events (and picks) only */
        ov_frame(m.wram, m.frame, ppu.pixels, 0, NULL, 0, NULL);
        ov_pick(m.frame, evs, tend);
        nfut = 0;
        return;
    }
    static OvWin win[OV_MAXWIN];
    /* the events up to this frame first (ov_pick needs the batch) */
    ov_frame(m.wram, m.frame, ppu.pixels, 0, win, 0, NULL);
    k = ov_pick(m.frame, evs, tend);
    if (k) {
        to_callback_point(&m);
        for (int i = 0; i < k; i++) future_start(&fut[i], evs[i], tend[i]);
        nfut = k;
    } else
        for (int i = 0; i < nfut; i++) future_step(&fut[i], m.frame - fut[i].f0);
    for (int i = 0; i < nfut; i++) {
        const Future *f = &fut[i];
        win[i] = (OvWin){f->p.pixels, f->ev, f->age, f->ended, f->age < f->n ? f->buf[f->age] : 0};
    }
    /* P1's pad this frame: the one the game latched */
    ov_frame(m.wram, m.frame, ppu.pixels, m.joy[0], win, nfut, canvas);
}

static void write_ppm(const char *dir, uint32_t frame)
{
    char path[512];
    snprintf(path, sizeof path, "%s/f%06u.ppm", dir, frame);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n256 224\n255\n");
    if (getenv("SBM2_PPUDUMP")) {   /* debugging the renderer: registers and memories */
        fprintf(stderr, "%s: BGMODE %02X TM %02X TS %02X M7SEL %02X M7 %d %d %d %d %d %d ofs %d %d\n", path, m.ppu[0x05],
                m.ppu[0x2C], m.ppu[0x2D], m.ppu[0x1A], ppu.m7[0], ppu.m7[1], ppu.m7[2], ppu.m7[3], ppu.m7[4], ppu.m7[5],
                ppu.m7hofs, ppu.m7vofs);
        char dp[600];
        snprintf(dp, sizeof dp, "%s.mem", path);
        FILE *d = fopen(dp, "wb");
        if (d) { fwrite(ppu.vram, 2, 0x8000, d); fwrite(ppu.cgram, 2, 256, d); fwrite(m.ppu, 1, 0x40, d); fclose(d); }
    }
    for (int i = 0; i < 256 * 224; i++) {
        uint32_t c = ppu.pixels[i];
        uint8_t px[3] = {c >> 16, c >> 8, c};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
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

/* Runs until the next vblank starts, or in frame-step mode to the next
 * wait-loop exit, then draws the frame from the registers as they are. */
static int run_frame(void)
{
    uint32_t f = m.frame;
    long n = 0;
    if (m.framestep) {
        do {
            snes_step(&m);
            if (++n > 50000000) return -1;
            if (m.pad_pos >= m.npads) return 1;
        } while (!(m.wait_at[m.cpu.pc] && snes_wait_point(&m, (uint32_t)m.cpu.pb << 16 | m.cpu.pc) == WAIT_EXIT));
        ppu_vblank(&m);
        for (int y = 1; y <= 224; y++) ppu_line(&m, y);
        return 0;
    }
    while (m.frame == f) {
        snes_step(&m);
        if (++n > 50000000) return -1;
    }
    static uint8_t last = 0xFF;
    if (m.wram[0x1A] != last && m.wram[0xBC]) {
        last = m.wram[0x1A];
        fprintf(stderr, "frame %u stage %02X (pad log %u/%u)\n", m.frame, last, m.pad_pos, m.npads);
    }
    if (m.pads && m.pad_pos >= m.npads) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: play <rom> [-i input] [-n frames] [-s f1,f2 -o dir] [-N] [-x hook]\n"); return 2; }
    const char *input = NULL, *replay = NULL, *outdir = ".", *shots = NULL, *timed = NULL;
    const char *ovfile = NULL, *video = NULL, *audio = NULL, *dump = NULL;
    long vfrom = 0, vto = -1, vstep = 1;
    long frames = -1;
    double fps = 60;
    int hooks = -1, uncapped = -1;
    uint8_t *enable = NULL;
    if (snes_load(&m, argv[1])) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) replay = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-O") && i + 1 < argc) ovfile = argv[++i];
        else if (!strcmp(argv[i], "-V") && i + 1 < argc) video = argv[++i];
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) vfrom = atol(argv[++i]);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) vto = atol(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) vstep = atol(argv[++i]);
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) audio = argv[++i];
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "-N")) hooks = 0;
        else if (!strcmp(argv[i], "-H")) hooks = 1;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) timed = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) { fps = atof(argv[++i]); uncapped = 0; }
        else if (!strcmp(argv[i], "-p")) uncapped = 0;
        else if (!strcmp(argv[i], "-u")) uncapped = 1;
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
    if (audio || dump) {
        if (audio && (!video || vstep != 1)) { fprintf(stderr, "-A needs -V and -k 1\n"); return 2; }
        sound = 1;
        spc_init(&spc);
        spc.debug = getenv("SBM2_SPCDBG") != NULL;
        snes_apu_tap = apu_tap;
    }
    m.input = pads;
    m.input_len = cap;
    if (replay) {
        uint32_t n;
        uint16_t *log = load_log(replay, &n);
        if (!log) { fprintf(stderr, "can't read %s\n", replay); return 2; }
        m.framestep = 1;
        m.pads = log;
        m.npads = n;
        for (uint32_t i = 0; i < n; i++) if (log[i] & 0xF) { m.pad_markers = 1; break; }
        input = replay;  /* the keyboard does not drive the pad */
    }
    if (uncapped < 0) uncapped = replay != NULL;
    if (hooks < 0) hooks = replay == NULL;
    if (hooks) hook_install(&m, enable);
    if (replay && timed) {
        /* The start state's pad log in frame-step (botrun's replay_words),
         * then the rest timed: a timed run plays back exactly. */
        uint32_t sn;
        uint16_t *sl = load_log(timed, &sn);
        if (!sl || sn > m.npads || memcmp(sl, m.pads, sn * 2)) { fprintf(stderr, "%s is not a prefix of %s\n", timed, replay); return 2; }
        uint16_t *log = (uint16_t *)m.pads;
        uint32_t n = m.npads;
        m.npads = sn;
        m.pad_pos = 0;
        m.pad_markers = 0;
        for (uint32_t i = 0; i < sn; i++) if (sl[i] & 0xF) { m.pad_markers = 1; break; }
        for (long steps = 0;; steps++) {
            uint32_t pc = (uint32_t)m.cpu.pb << 16 | m.cpu.pc;
            if (m.pad_pos >= m.npads && !m.fs_raised && !m.fs_in_nmi && !m.cpu.nmi &&
                m.wait_at[pc & 0xFFFF] && snes_wait_point(&m, pc) == WAIT_CMP)
                break;
            if (steps > 4000000000L) { fprintf(stderr, "replay of %s did not end\n", timed); return 2; }
            snes_step(&m);
        }
        m.framestep = 0;
        if (sound) {
            /* The prefix ran in frame-step: its fake APU answered at once
             * and frames took no real time, so the shadow is behind. Let it
             * catch up, then count time from here. */
            size_t left = spc_drain(&spc, 60ULL * 1024000);
            fprintf(stderr, "sound: prefix %ld handshakes, %ld timed out%s\n", spc.syncs, spc.timeouts,
                    left ? " (events left over)" : "");
            spc.delay = spc.max_lag = 0;
            spc.syncs = spc.timeouts = 0;
            spc_base = spc.cycles;
            clk_base = m.clock;
            spc.nsamples = 0;
        }
        m.pads = log + sn;
        m.npads = n - sn;
        m.pad_pos = 0;
        m.pad_markers = 0;
        free(sl);
    }

    static uint32_t canvas[OV_W * OV_H];
    if (ovfile) {
        int n = ov_load(ovfile);
        if (n < 0) { fprintf(stderr, "can't read %s\n", ovfile); return 2; }
        fprintf(stderr, "overlay: %d events\n", n);
    }
    if (video) {
        if (!ovfile) { fprintf(stderr, "-V needs -O\n"); return 2; }
        char cmd[2048];
        snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgr0 -s %dx%d -r 60 -i - "
                 "-c:v libx264 -preset faster -crf 18 -pix_fmt yuv420p '%s'", OV_W, OV_H, video);
        FILE *ff = popen(cmd, "w");
        if (!ff) { fprintf(stderr, "can't run ffmpeg\n"); return 2; }
        FILE *af = audio ? fopen(audio, "wb") : NULL;
        if (audio && !af) { fprintf(stderr, "can't write %s\n", audio); return 2; }
        long dump_at = dump ? atol(dump) : -1;
        long kept = 0, nsmp = 0;
        for (long f = 1;; f++) {
            int r = run_frame();
            if (r) break;
            if (sound) {
                spc_run_until(&spc, spc_time(&m));
                if (spc.debug && spc.delay > 1024 * 20) {
                    static uint32_t lastf;
                    if (m.frame > lastf + 600) fprintf(stderr, "sound lag %.0f ms at frame %u (stage %02X)\n", spc.delay / 1024.0, m.frame, m.wram[0x1A]);
                    lastf = m.frame;
                }
                if (dump_at >= 0 && (long)m.frame >= dump_at) {
                    static uint8_t img[0x10200];
                    const char *path = strchr(dump, ':');
                    FILE *df = path ? fopen(path + 1, "wb") : NULL;
                    if (df) fwrite(img, 1, spc_save(&spc, img), df), fclose(df);
                    else fprintf(stderr, "-D wants FRAME:FILE\n");
                    dump_at = -1;
                }
            }
            ppu.nodraw = (long)m.frame + 2 < vfrom;
            int keep = (long)m.frame >= vfrom && !(f % vstep) && !(vto >= 0 && (long)m.frame > vto);
            if (af && keep) {
                static int16_t out48[1600];
                resample_frame(spc.samples, spc.nsamples / 2, out48);
                fwrite(out48, 4, 800, af);
                nsmp += (long)spc.nsamples / 2;
            }
            spc.nsamples = 0;
            if ((long)m.frame < vfrom) { overlay_frame(NULL); continue; }
            overlay_frame(canvas);
            if (f % vstep) continue;
            if (vto >= 0 && (long)m.frame > vto) break;
            fwrite(canvas, 4, (size_t)OV_W * OV_H, ff);
            if (++kept % 3600 == 0) fprintf(stderr, "video: frame %u, %ld kept\n", m.frame, kept);
        }
        if (sound)
            fprintf(stderr, "sound: %ld samples for %ld frames (%.1f Hz at 60 fps); %ld handshakes, %ld timed out, "
                    "longest lag %.1f ms\n", nsmp, kept, kept ? nsmp * 60.0 / kept : 0.0, spc.syncs, spc.timeouts,
                    spc.max_lag / 1024.0);
        if (af) fclose(af);
        return pclose(ff) ? 1 : 0;
    }
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
            int r = run_frame();
            if (r > 0) break;
            if (r) { fprintf(stderr, "stuck at frame %ld\n", f); return 1; }
            for (const char *p = shots ? shots : ""; *p; ) {
                if (strtol(p, (char **)&p, 10) == f) write_ppm(outdir, m.frame);
                if (*p == ',') p++;
            }
        }
        return 0;
    }
#ifdef HAVE_SDL
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    int cw = ovfile ? OV_W : 256, ch = ovfile ? OV_H : 224;
    SDL_Window *win = SDL_CreateWindow("SBM2 port", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       ovfile ? OV_W * 2 / 3 : 256 * 3, ovfile ? OV_H * 2 / 3 : 224 * 3, SDL_WINDOW_RESIZABLE);
    /* Paced by the clock below, not vsync (a vsync present can block forever
     * on some X setups). */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, cw, ch);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, cw, ch);
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
        int fast = ks[SDL_SCANCODE_TAB] ? 8 : 1, r = 0;
        for (int k = 0; k < fast && !r; k++) {
            r = run_frame();
            if (!r && ovfile) overlay_frame(canvas);
        }
        if (r > 0) { printf("replay finished at frame %ld\n", f); break; }
        if (r) { fprintf(stderr, "stuck at frame %ld\n", f); return 1; }
        SDL_UpdateTexture(tex, NULL, ovfile ? canvas : ppu.pixels, cw * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        if (uncapped) continue;
        static double next;
        double now = (double)SDL_GetTicks64();
        next = next && next > now - 100 ? next + 1000 / fps : now + 1000 / fps;
        if (next > now + 1) SDL_Delay((uint32_t)(next - now));
    }
    SDL_Quit();
#endif
    return 0;
}
