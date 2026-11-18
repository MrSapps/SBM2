/* Overlays for build/port/play (-O): what the C++ agent did and thought,
 * drawn over a replay of its run. The agent run writes the overlay file
 * (agentrun with SBM2_OVERLAY, port/botrun.c); the replay is exact, so the
 * file's video frames line up with the replay's.
 *
 * The frame: the game at 4x on the left, a panel on the right. Over the
 * game: P1's trail, the oracle's look-ahead runs as they come (each run a
 * ghost path growing out from P1, red with a cross where P1 died in it,
 * cyan where it lived), and the move being played (a glowing path to its
 * end, bombs where it presses A). The panel: stage, the move's kind, the
 * agent's log, and counts. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overlay.h"
#include "ovfont.h"

enum { SCALE = 4, GX = 32, GY = 92, GW = 256 * SCALE, GH = 224 * SCALE, PX = GX + GW + 40 };

typedef struct { uint32_t vf; int kind; int dead; int n; int16_t *xy; char *text; uint16_t *pads; int npads; } Ev;
static Ev *ev;
static size_t nev, cap;

static void push(Ev e)
{
    if (nev == cap) { cap = cap ? 2 * cap : 4096; ev = realloc(ev, cap * sizeof *ev); }
    ev[nev++] = e;
}

int ov_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    static char line[1 << 20];
    while (fgets(line, sizeof line, f)) {
        Ev e = {0};
        char k = line[0];
        char *p = line + 2, *q;
        e.vf = (uint32_t)strtoul(p, &q, 10);
        p = q;
        e.kind = k;
        if (k == 'S' || k == 'P') {
            e.dead = (int)strtol(p, &q, 10);
            p = q;
            e.n = (int)strtol(p, &q, 10);
            p = q;
            e.xy = malloc((size_t)(e.n ? e.n : 1) * 4);
            for (int i = 0; i < 2 * e.n; i++) { e.xy[i] = (int16_t)strtol(p, &q, 10); p = q; }
            char *bar = strchr(p, '|');
            if (k == 'S' && bar) {
                /* pads, run-length " count:hex" */
                int cap = 0;
                for (p = bar + 1;;) {
                    long c = strtol(p, &q, 10);
                    if (q == p || *q != ':') break;
                    unsigned v = (unsigned)strtoul(q + 1, &p, 16);
                    if (e.npads + c > cap) { cap = (int)(e.npads + c) * 2; e.pads = realloc(e.pads, (size_t)cap * 2); }
                    for (long i = 0; i < c; i++) e.pads[e.npads++] = (uint16_t)v;
                }
            }
        } else if (k == 'A') {
            e.n = 1;
            e.xy = malloc(4);
            e.xy[0] = (int16_t)strtol(p, &q, 10);
            e.xy[1] = (int16_t)strtol(q, &q, 10);
        } else if (k == 'L' || k == 'W') {
            while (*p == ' ') p++;
            size_t n = strcspn(p, "\n");
            p[n] = 0;
            e.text = strdup(p);
        } else continue;
        push(e);
    }
    fclose(f);
    return (int)nev;
}

/* --- drawing on the canvas (XRGB8888, OV_W x OV_H) --- */
static uint32_t *cv;
static uint8_t cover[OV_W * OV_H];
static int bx0, by0, bx1, by1;

static void blend(int x, int y, uint32_t c, int a)
{
    if (x < 0 || y < 0 || x >= OV_W || y >= OV_H || a <= 0) return;
    if (a > 255) a = 255;
    uint32_t d = cv[y * OV_W + x];
    int r = (int)((d >> 16 & 255) * (255 - a) + (c >> 16 & 255) * a) / 255;
    int g = (int)((d >> 8 & 255) * (255 - a) + (c >> 8 & 255) * a) / 255;
    int b = (int)((d & 255) * (255 - a) + (c & 255) * a) / 255;
    cv[y * OV_W + x] = (uint32_t)(r << 16 | g << 8 | b);
}
static void fill(int x0, int y0, int w, int h, uint32_t c, int a)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) blend(x, y, c, a);
}
/* Coverage: stamps go into cover[] (max), then one blend: overlapping
 * stamps of one path don't darken each other. Clipped to the game area. */
static void cov_begin(void) { bx0 = OV_W; by0 = OV_H; bx1 = by1 = -1; }
static void stamp(double cx, double cy, double r)
{
    int x0 = (int)floor(cx - r - 1), x1 = (int)ceil(cx + r + 1), y0 = (int)floor(cy - r - 1), y1 = (int)ceil(cy + r + 1);
    if (x0 < GX) x0 = GX;
    if (y0 < GY) y0 = GY;
    if (x1 >= GX + GW) x1 = GX + GW - 1;
    if (y1 >= GY + GH) y1 = GY + GH - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double d = hypot(x + 0.5 - cx, y + 0.5 - cy);
            double a = r + 0.5 - d;
            if (a <= 0) continue;
            int v = a >= 1 ? 255 : (int)(a * 255);
            uint8_t *c = &cover[y * OV_W + x];
            if (v > *c) *c = (uint8_t)v;
        }
    if (x0 < bx0) bx0 = x0;
    if (y0 < by0) by0 = y0;
    if (x1 > bx1) bx1 = x1;
    if (y1 > by1) by1 = y1;
}
static void seg(double x0, double y0, double x1, double y1, double r)
{
    double len = hypot(x1 - x0, y1 - y0);
    int n = (int)(len / (r > 1.5 ? r * 0.7 : 1)) + 1;
    for (int i = 0; i <= n; i++) stamp(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, r);
}
static void cov_end(uint32_t c, int a)
{
    for (int y = by0; y <= by1; y++)
        for (int x = bx0; x <= bx1; x++) {
            uint8_t *v = &cover[y * OV_W + x];
            if (*v) { blend(x, y, c, *v * a / 255); *v = 0; }
        }
    cov_begin();
}

static void glyph(const OvFont *f, int x, int y, char ch, uint32_t c, int a)
{
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = f->a + (size_t)(ch - 32) * (size_t)(f->w * f->h);
    for (int j = 0; j < f->h; j++)
        for (int i = 0; i < f->w; i++)
            if (g[j * f->w + i]) blend(x + i, y + j, c, g[j * f->w + i] * a / 255);
}
/* Text, cut at maxw pixels (with "..."); returns the width drawn. */
static int text(const OvFont *f, int x, int y, const char *s, uint32_t c, int a, int maxw)
{
    int n = (int)strlen(s), fit = maxw / f->w;
    for (int i = 0; i < n && i < fit; i++) glyph(f, x + i * f->w, y, (n > fit && i >= fit - 3) ? '.' : s[i], c, a);
    return (n < fit ? n : fit) * f->w;
}
/* Text wrapped to lines of maxw pixels; returns the lines used (at most maxl). */
static int wrap(const OvFont *f, int x, int y, const char *s, uint32_t c, int a, int maxw, int maxl)
{
    int fit = maxw / f->w, l = 0, n = (int)strlen(s);
    for (int i = 0; i < n && l < maxl; l++) {
        int k = n - i <= fit ? n - i : fit;
        if (n - i > fit) for (int j = k; j > fit / 2; j--) if (s[i + j] == ' ') { k = j; break; }
        char b[512];
        snprintf(b, sizeof b, "%.*s", k, s + i);
        if (l == maxl - 1 && i + k < n && k > 3) b[k - 3] = b[k - 2] = b[k - 1] = '.';
        text(f, x, y + l * f->h, b, c, a, maxw);
        i += k;
        while (i < n && s[i] == ' ') i++;
    }
    return l;
}

/* --- the game's world pixels on the canvas --- */
/* World pixels are object pixels plus the origin ($5F01, $5EFB), which is
 * the camera (it follows BG2's scroll): world - origin is the screen. */
static int ox, oy;
static double wx2c(double x) { return GX + (x - ox) * SCALE; }
static double wy2c(double y) { return GY + (y - oy) * SCALE; }

static void path(const int16_t *xy, int n, double r)
{
    for (int i = 0; i + 1 < n; i++) {
        double x0 = wx2c(xy[2 * i]), y0 = wy2c(xy[2 * i + 1]), x1 = wx2c(xy[2 * i + 2]), y1 = wy2c(xy[2 * i + 3]);
        if (fabs(x1 - x0) > 48 * SCALE || fabs(y1 - y0) > 48 * SCALE) continue;   /* a wrap or a warp */
        seg(x0, y0, x1, y1, r);
    }
    if (n == 1) stamp(wx2c(xy[0]), wy2c(xy[1]), r);
}
static void cross(double x, double y, double s, double r)
{
    seg(x - s, y - s, x + s, y + s, r);
    seg(x - s, y + s, x + s, y - s, r);
}
static void ring(double x, double y, double rad, double r)
{
    for (int i = 0; i < 48; i++) {
        double a0 = i * 2 * M_PI / 48, a1 = (i + 1) * 2 * M_PI / 48;
        seg(x + rad * cos(a0), y + rad * sin(a0), x + rad * cos(a1), y + rad * sin(a1), r);
    }
}

/* --- state over the replay --- */
enum { TRAIL = 300, GHOST_LIFE = 50, GROW = 8, LOGN = 18 };
static int16_t trail[TRAIL][2];
static uint32_t trail_vf[TRAIL];
static int ntrail, trail_at;
static size_t evi;               /* next event to take */
static size_t ghost0;            /* first S event still shown */
static const Ev *plan;           /* the move being played */
static int plan_at;              /* the point of it P1 is at */
static uint32_t plan_vf;
static const Ev *bombs[16];
static int nbombs;
static const char *what = "";
static const Ev *logs[LOGN];
static uint32_t logvf[LOGN];
static int nlog;
static long nsims, ndied, nmoves, stage_sims;
static int last_stage = -1;
static int dec_live, dec_dead;   /* the last decision's runs */
static uint32_t dec_vf;
enum { HIST = 90 };
static int hist_live[HIST], hist_dead[HIST], nhist;

static int goal_seq, win_goal = -1;
static size_t batch0;            /* the first S event of the newest batch */
static uint32_t batch_vf = 0xFFFFFFFF;

const uint16_t *ov_pads(int e, int *n) { *n = ev[e].npads; return ev[e].pads; }
int ov_dead(int e) { return ev[e].dead; }

/* The run the move came from: of the decision's runs, the one whose path
 * is closest to the plan's (the plan starts a frame in, at == 1; a survive
 * plays only the first 8..60 frames of its run). */
static int chosen_ev = -1;
static uint32_t chosen_vf = 0xFFFFFFFF;
static int is_plan(const Ev *e)
{
    if (!plan || plan->vf != e->vf || !e->n) return 0;
    if (chosen_vf != plan->vf) {
        chosen_vf = plan->vf;
        chosen_ev = -1;
        long best = -1;
        for (size_t i = batch0; i < evi; i++) {
            const Ev *c = &ev[i];
            if (c->kind != 'S' || c->vf != plan->vf || !c->n) continue;
            int n = plan->n < c->n ? plan->n : c->n;
            long d = 0;
            for (int k = 0; k < n; k++) d += abs(plan->xy[2 * k] - c->xy[2 * k]) + abs(plan->xy[2 * k + 1] - c->xy[2 * k + 1]);
            d = d * 64 / (n ? n : 1) + (c->dead >= 0) * 1000;   /* (a run that dies was not played to its death) */
            if (best < 0 || d < best) { best = d; chosen_ev = (int)i; }
        }
    }
    return e == &ev[chosen_ev < 0 ? 0 : chosen_ev] && chosen_ev >= 0;
}

int ov_pick(uint32_t vf, int all_ended, int *evs)
{
    if (batch_vf != vf) return 0;
    if (!all_ended && win_goal == goal_seq) return 0;
    int n = 0;
    /* the plan's run first, then deaths (soonest first), then other runs that lived */
    for (size_t i = batch0; i < evi && n < OV_MAXWIN; i++)
        if (ev[i].kind == 'S' && ev[i].npads && is_plan(&ev[i])) evs[n++] = (int)i;
    for (int pass = 0; pass < 2; pass++)
        for (int left = OV_MAXWIN - n; left > 0; left = OV_MAXWIN - n) {
            int best = -1;
            for (size_t i = batch0; i < evi; i++) {
                const Ev *e = &ev[i];
                if (e->kind != 'S' || !e->npads || (pass == 0) != (e->dead >= 0)) continue;
                int taken = 0;
                for (int k = 0; k < n; k++) taken |= evs[k] == (int)i;
                if (taken) continue;
                if (best < 0 || (pass == 0 && e->dead > ev[best].dead)) best = (int)i;   /* (latest death first: the longest story) */
                if (pass == 1 && best >= 0) break;
            }
            if (best < 0) break;
            evs[n++] = best;
        }
    if (n) win_goal = goal_seq;
    return n;
}

static int rd16(const uint8_t *w, int a) { return (int16_t)(w[a] | w[a + 1] << 8); }

/* A pad: the d-pad and A/B (Y/X dim), lit where pressed; u = unit pixels. */
static void padicon(int x, int y, int u, uint16_t pad)
{
    fill(x - u, y - u, 13 * u, 7 * u, 0x000000, 150);
    const uint32_t on = 0xFFE060, off = 0x506070;
    /* d-pad: centre at (x+2u, y+2.5u) */
    int cx = x + 2 * u, cy = y + 2 * u + u / 2;
    fill(cx - u / 2, cy - u / 2, u, u, off, 255);
    fill(cx - u / 2, cy - 3 * u / 2 - u / 4, u, u, pad & 0x0800 ? on : off, 255);   /* up */
    fill(cx - u / 2, cy + u / 2 + u / 4, u, u, pad & 0x0400 ? on : off, 255);       /* down */
    fill(cx - 3 * u / 2 - u / 4, cy - u / 2, u, u, pad & 0x0200 ? on : off, 255);   /* left */
    fill(cx + u / 2 + u / 4, cy - u / 2, u, u, pad & 0x0100 ? on : off, 255);       /* right */
    /* buttons: X top, Y left, A right, B bottom (SNES layout) */
    int bx = x + 9 * u, by = cy;
    struct { int dx, dy; uint16_t bit; uint32_t c; } bt[] = {
        {0, -1, 0x0040, 0x6060FF}, {-1, 0, 0x4000, 0x40C040}, {1, 0, 0x0080, 0xFF4040}, {0, 1, 0x8000, 0xFFC020}};
    for (int i = 0; i < 4; i++) {
        double px = bx + bt[i].dx * 1.3 * u, py = by + bt[i].dy * 1.3 * u;
        int lit = (pad & bt[i].bit) != 0;
        for (int yy = (int)(py - u / 2.0 - 1); yy <= (int)(py + u / 2.0 + 1); yy++)
            for (int xx = (int)(px - u / 2.0 - 1); xx <= (int)(px + u / 2.0 + 1); xx++) {
                double d = hypot(xx + 0.5 - px, yy + 0.5 - py), a = u / 2.0 + 0.5 - d;
                if (a > 0) blend(xx, yy, lit ? 0xFFFFFF : bt[i].c, (int)((a > 1 ? 1 : a) * (lit ? 255 : 90)));
            }
    }
}

void ov_frame(const uint8_t *wram, uint32_t vf, const uint32_t *game, uint16_t pad, const OvWin *win, int nwin,
              uint32_t *canvas)
{
    ox = rd16(wram, 0x5F01);
    oy = rd16(wram, 0x5EFB);
    int stage = wram[0x1A];
    int inStage = wram[0xBC] != 0;
    int px = rd16(wram, 0x8010) + ox, py = rd16(wram, 0x8018) + oy;
    if (stage != last_stage) { last_stage = stage; ntrail = 0; stage_sims = 0; plan = NULL; nbombs = 0; }

    /* take this frame's events */
    for (; evi < nev && ev[evi].vf <= vf; evi++) {
        const Ev *e = &ev[evi];
        if (e->kind == 'S') {
            if (e->vf != batch_vf) { batch_vf = e->vf; batch0 = evi; }
            nsims++; stage_sims++;
            if (e->dead >= 0) ndied++;
            if (e->vf != dec_vf) {
                if (dec_live + dec_dead) {
                    memmove(hist_live, hist_live + 1, (HIST - 1) * sizeof *hist_live);
                    memmove(hist_dead, hist_dead + 1, (HIST - 1) * sizeof *hist_dead);
                    hist_live[HIST - 1] = dec_live; hist_dead[HIST - 1] = dec_dead;
                    if (nhist < HIST) nhist++;
                }
                dec_vf = e->vf; dec_live = dec_dead = 0;
            }
            if (e->dead >= 0) dec_dead++; else dec_live++;
        } else if (e->kind == 'P') {
            plan = e; plan_at = 0; plan_vf = e->vf; nbombs = 0; nmoves++;
        } else if (e->kind == 'A') {
            if (nbombs < 16) bombs[nbombs++] = e;
        } else if (e->kind == 'W') {
            what = e->text;
            goal_seq++;
        } else if (e->kind == 'L') {
            memmove(logs + 1, logs, (LOGN - 1) * sizeof *logs);
            memmove(logvf + 1, logvf, (LOGN - 1) * sizeof *logvf);
            logs[0] = e; logvf[0] = vf;
            if (nlog < LOGN) nlog++;
        }
    }
    while (ghost0 < evi && (ev[ghost0].kind != 'S' || ev[ghost0].vf + GHOST_LIFE < vf)) ghost0++;

    if (!canvas) return;
    cv = canvas;
    cov_begin();
    /* background */
    static uint32_t *bgimg;
    if (!bgimg) {
        bgimg = malloc((size_t)OV_W * OV_H * 4);
        for (int y = 0; y < OV_H; y++)
            for (int x = 0; x < OV_W; x++) {
                int v = 14 + (y * 10 / OV_H);
                bgimg[y * OV_W + x] = (uint32_t)(v << 16 | v << 8 | (v + 10));
            }
    }
    memcpy(cv, bgimg, (size_t)OV_W * OV_H * 4);
    /* the game, 4x */
    for (int y = 0; y < GH; y++) {
        const uint32_t *row = game + (y / SCALE) * 256;
        uint32_t *o = cv + (GY + y) * OV_W + GX;
        for (int x = 0; x < GW; x++) o[x] = row[x / SCALE] & 0xFFFFFF;
    }
    fill(GX - 3, GY - 3, GW + 6, 3, 0x40C0FF, 160);
    fill(GX - 3, GY + GH, GW + 6, 3, 0x40C0FF, 160);
    fill(GX - 3, GY, 3, GH, 0x40C0FF, 160);
    fill(GX + GW, GY, 3, GH, 0x40C0FF, 160);
    if (inStage) {
        /* P1's trail */
        if (!ntrail || trail[(trail_at + TRAIL - 1) % TRAIL][0] != px || trail[(trail_at + TRAIL - 1) % TRAIL][1] != py) {
            trail[trail_at][0] = (int16_t)px; trail[trail_at][1] = (int16_t)py; trail_vf[trail_at] = vf;
            trail_at = (trail_at + 1) % TRAIL;
            if (ntrail < TRAIL) ntrail++;
        }
        for (int k = 1; k < ntrail; k++) {
            int i0 = (trail_at - ntrail + k - 1 + 2 * TRAIL) % TRAIL, i1 = (trail_at - ntrail + k + 2 * TRAIL) % TRAIL;
            int16_t pts[4] = {trail[i0][0], trail[i0][1], trail[i1][0], trail[i1][1]};
            path(pts, 2, 3.0);
            cov_end(0x6080FF, 20 + 150 * k / ntrail);
        }
        /* the look-ahead runs: ghosts growing from where P1 was */
        for (size_t i = ghost0; i < evi; i++) {
            const Ev *e = &ev[i];
            if (e->kind != 'S' || !e->n) continue;
            int age = (int)(vf - e->vf);
            int shown = (age + 1) * GROW;
            if (shown > e->n) shown = e->n;
            int fade = age < GHOST_LIFE - 20 ? 255 : 255 * (GHOST_LIFE - age) / 20;
            uint32_t col = e->dead >= 0 ? 0xFF3060 : e->dead == -2 ? 0xFFD040 : 0x30E0FF;
            path(e->xy, shown, 5);
            cov_end(col, 40 * fade / 255);
            path(e->xy, shown, 1.8);
            cov_end(col, 170 * fade / 255);
            if (shown > 0) {
                double hx = wx2c(e->xy[2 * (shown - 1)]), hy = wy2c(e->xy[2 * (shown - 1) + 1]);
                if (e->dead >= 0 && shown == e->n) {
                    cross(hx, hy, 9, 2.2);
                    cov_end(0xFF3050, 230 * fade / 255);
                } else {
                    stamp(hx, hy, 3.2);
                    cov_end(col, 200 * fade / 255);
                }
            }
        }
        /* the move: the rest of its path */
        if (plan && plan->n) {
            int best = plan_at, bd = 1 << 30;
            for (int i = plan_at; i < plan->n && i < plan_at + 40; i++) {
                int d = abs(plan->xy[2 * i] - px) + abs(plan->xy[2 * i + 1] - py);
                if (d < bd) { bd = d; best = i; }
            }
            plan_at = best;
            int rest = plan->n - plan_at;
            if (rest > 1 && bd < 24) {
                path(plan->xy + 2 * plan_at, rest, 7);
                cov_end(0xFFE060, 50);
                path(plan->xy + 2 * plan_at, rest, 2.6);
                cov_end(0xFFF0A0, 230);
                double ex = wx2c(plan->xy[2 * (plan->n - 1)]), ey = wy2c(plan->xy[2 * (plan->n - 1) + 1]);
                double pulse = 1 + 0.15 * sin((vf - plan_vf) * 0.25);
                ring(ex, ey, 22 * pulse, 2.4);
                cov_end(0xFFF0A0, 220);
            }
            for (int b = 0; b < nbombs; b++) {
                double x = wx2c(bombs[b]->xy[0]), y = wy2c(bombs[b]->xy[1]);
                int passed = 0;
                for (int i = 0; i < plan_at && i < plan->n; i++)
                    if (plan->xy[2 * i] == bombs[b]->xy[0] && plan->xy[2 * i + 1] == bombs[b]->xy[1]) { passed = 1; break; }
                if (passed && bd < 24) continue;
                ring(x, y, 16, 3);
                cov_end(0xFF8030, 230);
                stamp(x, y, 8);
                cov_end(0xFF8030, 120);
            }
        }
    }

    /* the panel */
    int y;
    char b[256];
    int pw = OV_W - PX - 32;
    text(&ov_huge, PX, 18, "BOMBER AGENT", 0xFFFFFF, 255, pw);
    text(&ov_small, PX + 2, 70, "GOAP planner + exact look-ahead on the emulator", 0x80A0C0, 255, pw);
    y = GY - 2;
    if (stage >= 0x10 && stage < 0x60) snprintf(b, sizeof b, "STAGE %d-%d", stage >> 4, (stage & 15) + 1);
    else snprintf(b, sizeof b, "STAGE --");
    text(&ov_big, PX, y, b, 0xFFE060, 255, 400);
    snprintf(b, sizeof b, "frame %u", vf);
    text(&ov_mid, PX + 290, y + 6, b, 0xA0B0C0, 255, 300);
    y += 46;
    fill(PX, y, pw, 2, 0x40C0FF, 120);
    y += 10;
    /* the move's goal, big: up to the ':' ("kill 5,10 (odds ...)"), then the how */
    {
        char g[256];
        snprintf(g, sizeof g, "%s", what);
        char *how = strchr(g, ':');
        if (how) *how++ = 0;
        char *paren = strstr(g, " (");
        if (paren) *paren = 0;
        for (char *c = g; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        text(&ov_small, PX, y, "NOW", 0x80A0C0, 255, 60);
        y += ov_small.h;
        y += wrap(&ov_big, PX, y, g, 0xFFFFFF, 255, pw, 1) * ov_big.h;
        if (how) {
            while (*how == ' ') how++;
            y += wrap(&ov_mid, PX, y, how, 0xFFF0A0, 255, pw, 1) * ov_mid.h;
        } else y += ov_mid.h;
    }
    y += 8;
    /* look-ahead: the last decision's runs */
    {
        int tot = dec_live + dec_dead;
        int age = (int)(vf - dec_vf), a = age < 120 ? 255 : 120;
        snprintf(b, sizeof b, "LOOK-AHEAD  %d futures: %d live, %d die", tot, dec_live, dec_dead);
        text(&ov_small, PX, y, b, 0xC0D0E0, a, pw);
        y += ov_small.h + 2;
        if (tot) {
            int lw = pw * dec_live / tot;
            fill(PX, y, lw, 12, 0x30E0FF, 200 * a / 255);
            fill(PX + lw, y, pw - lw, 12, 0xFF3060, 200 * a / 255);
        } else fill(PX, y, pw, 12, 0x405060, 120);
        y += 18;
        snprintf(b, sizeof b, "simulated %ld this stage, %ld in all (%ld ended in a death)", stage_sims, nsims, ndied);
        text(&ov_small, PX, y, b, 0x90A0B0, 255, pw);
        y += ov_small.h + 8;
    }
    /* futures: the chosen runs, replayed from the decision */
    {
        const int ww = 368, wh = 322, gap = 16;
        text(&ov_mid, PX, y, "FUTURES", 0x80A0C0, 255, 200);
        text(&ov_small, PX + 120, y + 4, "what each look-ahead saw, frame for frame", 0x708090, 255, pw - 120);
        y += ov_mid.h + 6;
        for (int k = 0; k < OV_MAXWIN; k++) {
            int x0 = PX + (k & 1) * (ww + gap), y0 = y + (k >> 1) * (wh + gap);
            if (k >= nwin) { fill(x0, y0, ww, wh, 0x202838, 160); continue; }
            const OvWin *w = &win[k];
            const Ev *e = &ev[w->ev];
            int dead = e->dead, died = dead >= 0 && w->age >= dead + 40;   /* (the death plays first) */
            for (int yy = 0; yy < wh; yy++) {
                const uint32_t *row = w->pix + (yy * 224 / wh) * 256;
                uint32_t *o = cv + (y0 + yy) * OV_W + x0;
                for (int xx = 0; xx < ww; xx++) {
                    uint32_t c = row[xx * 256 / ww] & 0xFFFFFF;
                    if (died || w->ended) {   /* hold: dim, red if dead */
                        int r = c >> 16 & 255, g = c >> 8 & 255, bb = c & 255, l = (r * 3 + g * 6 + bb) / 10;
                        c = died ? (uint32_t)((l / 2 + 110) << 16 | (l / 3) << 8 | (l / 3)) : (uint32_t)((l / 2) << 16 | (l / 2) << 8 | (l / 2 + 20));
                    }
                    o[xx] = c;
                }
            }
            int pl = is_plan(e);
            uint32_t bc = pl ? 0xFFE060 : dead >= 0 ? 0xFF3060 : 0x30E0FF;
            fill(x0 - 3, y0 - 3, ww + 6, 3, bc, 230);
            fill(x0 - 3, y0 + wh, ww + 6, 3, bc, 230);
            fill(x0 - 3, y0, 3, wh, bc, 230);
            fill(x0 + ww, y0, 3, wh, bc, 230);
            fill(x0, y0, ww, ov_small.h + 4, 0x000000, 170);
            text(&ov_small, x0 + 6, y0 + 2, pl ? "CHOSEN" : dead >= 0 ? "ALTERNATIVE" : "ALTERNATIVE", bc, 255, 140);
            if (dead >= 0 && w->age >= dead && !died) snprintf(b, sizeof b, "DIES");
            else if (dead >= 0 && !died) snprintf(b, sizeof b, "dies in %.1fs", (dead - w->age) / 60.0);
            else if (died) snprintf(b, sizeof b, "DEAD at +%.1fs", dead / 60.0);
            else if (dead == -2) snprintf(b, sizeof b, "exits");
            else snprintf(b, sizeof b, "lives  +%.1fs", w->age / 60.0);
            text(&ov_small, x0 + ww - 6 - (int)strlen(b) * ov_small.w, y0 + 2, b, died ? 0xFF6070 : 0xE0E8F0, 255, 200);
            if (!died && !w->ended) padicon(x0 + 8, y0 + wh - 7 * 6 + 2, 6, w->pad);
            if (died) {
                int tw = 4 * ov_huge.w;
                text(&ov_huge, x0 + (ww - tw) / 2, y0 + wh / 2 - ov_huge.h / 2, "DEAD", 0xFF4050, 255, 200);
            }
        }
    }
    /* under the game: each decision's futures, oldest left (cyan lived, red died) */
    {
        int x0 = GX, y0 = GY + GH + 12, w = GW - 150, hh = 56, bw = w / HIST;
        text(&ov_small, GX + GW - 120, y0, "PAD", 0x80A0C0, 255, 60);
        padicon(GX + GW - 13 * 9 + 9, y0 + ov_small.h + 4, 9, pad);
        text(&ov_small, x0, y0, "DECISIONS", 0x80A0C0, 255, 200);
        y0 += ov_small.h + 2;
        fill(x0, y0, w, hh, 0x203040, 120);
        for (int i = 0; i < HIST; i++) {
            if (i < HIST - nhist) continue;
            int L = hist_live[i], D = hist_dead[i];
            int lh = L > 24 ? hh / 2 : L * hh / 48, dh = D > 24 ? hh / 2 : D * hh / 48;
            fill(x0 + i * bw + 1, y0 + hh / 2 - lh, bw - 2, lh, 0x30E0FF, 210);
            fill(x0 + i * bw + 1, y0 + hh / 2, bw - 2, dh, 0xFF3060, 210);
        }
        fill(x0, y0 + hh / 2, w, 1, 0x80A0C0, 160);
    }
    /* over the game: the newest log line, and the legend */
    if (nlog) {
        char tb[400];
        const char *t = logs[0]->text;
        while (*t == ' ' || (*t >= '0' && *t <= '9')) t++;
        snprintf(tb, sizeof tb, "%s", t);
        char *tail = strstr(tb, " frames; option");
        if (tail) {
            while (tail > tb && *tail != '(') tail--;
            *tail = 0;
            for (char *e = tail - 1; e >= tb && *e == ' '; e--) *e = 0;
        }
        int age = (int)(vf - logvf[0]);
        text(&ov_mid, GX, GY - 40, tb, strstr(tb, "dies") ? 0xFF8090 : 0xE0E8F0, age < 30 ? 255 : 200, GW);
    }
    {
        int x = GX, yl = 16;
        struct { uint32_t c; const char *s; } lg[] = {{0xFFF0A0, "move"}, {0xFF8030, "bomb"}, {0x30E0FF, "future: lives"},
                                                      {0xFF3060, "future: dies"}, {0x6080FF, "trail"}};
        for (size_t i = 0; i < sizeof lg / sizeof lg[0]; i++) {
            fill(x, yl + 7, 18, 6, lg[i].c, 230);
            x += 24;
            x += text(&ov_small, x, yl, lg[i].s, 0xC0D0E0, 255, 300) + 22;
        }
    }
}
