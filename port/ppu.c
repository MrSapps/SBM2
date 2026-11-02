/* PPU memory and scanline renderer for the front-end (see ppu.h). Register
 * meanings follow the usual SNES documentation (fullsnes); only what this
 * game needs is drawn. */
#include <string.h>
#include "ppu.h"
#include "snes.h"

void ppu_write(Snes *s, uint8_t r, uint8_t v)
{
    Ppu *p = s->ppu_state;
    if (!p) return;
    switch (r) {
    case 0x02: p->oamreload = (p->oamreload & 0x200) | v << 1; p->oamaddr = p->oamreload; break;
    case 0x03: p->oamreload = (p->oamreload & 0x1FE) | (v & 1) << 9; p->oamaddr = p->oamreload; break;
    case 0x04:
        if (p->oamaddr < 0x200) {
            if (!(p->oamaddr & 1)) p->oamlatch = v;
            else { p->oam[p->oamaddr - 1] = p->oamlatch; p->oam[p->oamaddr] = v; }
        } else {
            p->oam[0x200 + (p->oamaddr & 0x1F)] = v;
        }
        p->oamaddr = (p->oamaddr + 1) & 0x3FF;
        break;
    case 0x0D: case 0x0F: case 0x11: case 0x13: {  /* BGnHOFS */
        int n = (r - 0x0D) >> 1;
        p->hofs[n] = (v << 8 | (p->ofslatch & ~7) | (p->hofslatch & 7)) & 0x3FF;
        p->ofslatch = p->hofslatch = v;
        break;
    }
    case 0x0E: case 0x10: case 0x12: case 0x14: {  /* BGnVOFS */
        int n = (r - 0x0E) >> 1;
        p->vofs[n] = (v << 8 | p->ofslatch) & 0x3FF;
        p->ofslatch = v;
        break;
    }
    case 0x16: p->vaddr = (p->vaddr & 0xFF00) | v; break;
    case 0x17: p->vaddr = (p->vaddr & 0x00FF) | v << 8; break;
    case 0x18: case 0x19: {
        uint8_t vmain = s->ppu[0x15];
        uint16_t a = p->vaddr;
        switch ((vmain >> 2) & 3) {  /* address remapping */
        case 1: a = (a & 0xFF00) | (a & 0x00E0) >> 5 | (a & 0x001F) << 3; break;
        case 2: a = (a & 0xFE00) | (a & 0x01C0) >> 6 | (a & 0x003F) << 3; break;
        case 3: a = (a & 0xFC00) | (a & 0x0380) >> 7 | (a & 0x007F) << 3; break;
        }
        a &= 0x7FFF;
        if (r == 0x18) p->vram[a] = (p->vram[a] & 0xFF00) | v;
        else p->vram[a] = (p->vram[a] & 0x00FF) | v << 8;
        if ((r == 0x19) == !!(vmain & 0x80)) {
            static const uint16_t inc[4] = {1, 32, 128, 128};
            p->vaddr += inc[vmain & 3];
        }
        break;
    }
    case 0x21: p->cgaddr = v; p->cgflip = 0; break;
    case 0x22:
        if (!p->cgflip) { p->cglatch = v; p->cgflip = 1; }
        else { p->cgram[p->cgaddr++] = (v << 8 | p->cglatch) & 0x7FFF; p->cgflip = 0; }
        break;
    case 0x32:  /* COLDATA: fixed colour */
        if (v & 0x20) p->fixed = (p->fixed & ~0x001F) | (v & 0x1F);
        if (v & 0x40) p->fixed = (p->fixed & ~0x03E0) | (v & 0x1F) << 5;
        if (v & 0x80) p->fixed = (p->fixed & ~0x7C00) | (v & 0x1F) << 10;
        break;
    }
}

void ppu_vblank(Snes *s)
{
    Ppu *p = s->ppu_state;
    if (p && !(s->ppu[0] & 0x80)) p->oamaddr = p->oamreload;
}

/* One layer's line: colour index (0 = transparent) and priority bit. */
typedef struct { uint8_t col[256], pri[256]; uint8_t obj_math[256]; } Line;

static void bg_line(Snes *s, Ppu *p, int n, int bpp, int palbase, int y, Line *out)
{
    uint8_t sc = s->ppu[0x07 + n];
    uint16_t mapbase = (sc & 0xFC) << 8;
    uint16_t chrbase = ((s->ppu[0x0B + (n >> 1)] >> ((n & 1) * 4)) & 0x0F) << 12;
    int big = (s->ppu[0x05] >> (4 + n)) & 1, ts = big ? 16 : 8;
    int yy = (y + p->vofs[n]) & 0x3FF;
    for (int x = 0; x < 256; x++) {
        int xx = (x + p->hofs[n]) & 0x3FF;
        int tx = xx / ts, ty = yy / ts;
        uint16_t a = mapbase + (ty & 31) * 32 + (tx & 31);
        if ((tx & 32) && (sc & 1)) a += 0x400;
        if (ty & 32) a += (sc & 3) == 3 ? 0x800 : (sc & 2) ? 0x400 : 0;
        uint16_t e = p->vram[a & 0x7FFF];
        int px = xx % ts, py = yy % ts;
        if (e & 0x4000) px = ts - 1 - px;
        if (e & 0x8000) py = ts - 1 - py;
        uint16_t ch = (e & 0x3FF) + (px >> 3) + (py >> 3) * 16;
        uint16_t ca = chrbase + (ch & 0x3FF) * (bpp * 4) + (py & 7);
        int bit = 7 - (px & 7), c = 0;
        for (int pl = 0; pl < bpp; pl += 2) {
            uint16_t w = p->vram[(ca + pl * 4) & 0x7FFF];
            c |= ((w >> bit) & 1) << pl | ((w >> (8 + bit)) & 1) << (pl + 1);
        }
        out->col[x] = c ? palbase + ((e >> 10) & 7) * (1 << bpp) + c : 0;
        out->pri[x] = (e >> 13) & 1;
        if (!c) out->col[x] = 0;
    }
}

static void obj_line(Snes *s, Ppu *p, int y, Line *out)
{
    static const uint8_t sizes[8][2][2] = {
        {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
        {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
    };
    uint8_t obsel = s->ppu[0x01];
    uint16_t base = (obsel & 7) << 13, gap = (((obsel >> 3) & 3) + 1) << 12;
    memset(out, 0, sizeof *out);
    uint8_t done[256] = {0};
    for (int i = 0; i < 128; i++) {
        const uint8_t *o = &p->oam[i * 4];
        uint8_t hi = p->oam[0x200 + i / 4] >> ((i & 3) * 2);
        int w = sizes[obsel >> 5][hi >> 1 & 1][0], h = sizes[obsel >> 5][hi >> 1 & 1][1];
        int ry = (y - o[1]) & 0xFF;
        if (ry >= h) continue;
        int x0 = o[0] | (hi & 1) << 8;
        if (x0 >= 256) x0 -= 512;
        uint8_t at = o[3];
        if (at & 0x80) ry = h - 1 - ry;
        for (int rx = 0; rx < w; rx++) {
            int x = x0 + rx;
            if (x < 0 || x > 255 || done[x]) continue;
            int sx = (at & 0x40) ? w - 1 - rx : rx;
            int ch = (o[2] & 0xF0) + (((o[2] & 0x0F) + (sx >> 3)) & 0x0F) + (ry >> 3) * 16;
            uint16_t ca = base + ((at & 1) ? gap : 0) + (ch & 0xFF) * 16 + (ry & 7);
            int bit = 7 - (sx & 7), c = 0;
            for (int pl = 0; pl < 4; pl += 2) {
                uint16_t wd = p->vram[(ca + pl * 4) & 0x7FFF];
                c |= ((wd >> bit) & 1) << pl | ((wd >> (8 + bit)) & 1) << (pl + 1);
            }
            if (!c) continue;
            done[x] = 1;
            int pal = (at >> 1) & 7;
            out->col[x] = 128 + pal * 16 + c;
            out->pri[x] = (at >> 4) & 3;
            out->obj_math[x] = pal >= 4;
        }
    }
}

/* Window mask for a layer's 4-bit W12SEL nibble and 2-bit logic. */
static int in_window(Snes *s, int sel, int logic, int x)
{
    int e1 = sel & 2, e2 = sel & 8;
    if (!e1 && !e2) return 0;
    int w1 = x >= s->ppu[0x26] && x <= s->ppu[0x27], w2 = x >= s->ppu[0x28] && x <= s->ppu[0x29];
    if (sel & 1) w1 = !w1;
    if (sel & 4) w2 = !w2;
    if (!e2) return w1;
    if (!e1) return w2;
    switch (logic & 3) {
    case 0: return w1 | w2;
    case 1: return w1 & w2;
    case 2: return w1 ^ w2;
    default: return !(w1 ^ w2);
    }
}

static int win_sel(Snes *s, int layer)  /* 0-3 BG, 4 OBJ, 5 colour */
{
    return (s->ppu[0x23 + layer / 2] >> ((layer & 1) * 4)) & 0x0F;
}

static int win_logic(Snes *s, int layer)
{
    return layer < 4 ? (s->ppu[0x2A] >> (layer * 2)) & 3 : (s->ppu[0x2B] >> ((layer - 4) * 2)) & 3;
}

/* Priority order, front to back: layer (0-3 BG, 4 OBJ) and priority. */
typedef struct { uint8_t layer, pri; } Slot;
static const Slot order0[] = {{4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {2, 1}, {3, 1}, {4, 0}, {2, 0}, {3, 0}};
static const Slot order1[] = {{4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {2, 1}, {4, 0}, {2, 0}};
static const Slot order1h[] = {{2, 1}, {4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {4, 0}, {2, 0}};

/* Front pixel of a screen: colour index, layer (5 = backdrop), OBJ maths flag. */
static void compose(Snes *s, Line *l, uint8_t enable, uint8_t winmask, int x,
                    const Slot *ord, int n, int *col, int *layer, int *objm)
{
    for (int i = 0; i < n; i++) {
        int L = ord[i].layer;
        if (!(enable & (1 << L)) || !l[L].col[x] || l[L].pri[x] != ord[i].pri) continue;
        if ((winmask & (1 << L)) && in_window(s, win_sel(s, L), win_logic(s, L), x)) continue;
        *col = l[L].col[x]; *layer = L; *objm = l[L].obj_math[x];
        return;
    }
    *col = 0; *layer = 5; *objm = 0;
}

static uint32_t rgb(uint16_t c, int bright)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = r * bright / 15; g = g * bright / 15; b = b * bright / 15;
    return (uint32_t)(r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

void ppu_line(Snes *s, int y)
{
    Ppu *p = s->ppu_state;
    if (!p || y < 1 || y > 224) return;
    uint32_t *row = &p->pixels[(y - 1) * 256];
    uint8_t ini = s->ppu[0x00];
    if (ini & 0x80) { memset(row, 0, 256 * 4); return; }
    int bright = ini & 0x0F, mode = s->ppu[0x05] & 7;
    static Line l[5];
    memset(l, 0, sizeof l[0] * 4);
    const Slot *ord; int nord;
    uint8_t used = s->ppu[0x2C] | s->ppu[0x2D];
    if (mode == 0) {
        for (int n = 0; n < 4; n++) if (used & (1 << n)) bg_line(s, p, n, 2, n * 32, y, &l[n]);
        ord = order0; nord = 12;
    } else {  /* mode 1 (anything else is drawn as mode 1) */
        if (used & 1) bg_line(s, p, 0, 4, 0, y, &l[0]);
        if (used & 2) bg_line(s, p, 1, 4, 0, y, &l[1]);
        if (used & 4) bg_line(s, p, 2, 2, 0, y, &l[2]);
        if (s->ppu[0x05] & 8) { ord = order1h; nord = 10; } else { ord = order1; nord = 10; }
    }
    if (used & 0x10) obj_line(s, p, y, &l[4]);
    uint8_t cgwsel = s->ppu[0x30], cgadsub = s->ppu[0x31];
    for (int x = 0; x < 256; x++) {
        int mc, ml, mo, sc, sl, so;
        compose(s, l, s->ppu[0x2C], s->ppu[0x2E], x, ord, nord, &mc, &ml, &mo);
        uint16_t main = p->cgram[mc], sub;
        int cwin = in_window(s, win_sel(s, 5), win_logic(s, 5), x);
        /* regions: 0 never, 1 outside the colour window, 2 inside, 3 always */
        int black = cgwsel >> 6, prevent = (cgwsel >> 4) & 3;
        int clip = black == 3 || (black == 1 && !cwin) || (black == 2 && cwin);
        int nomath = prevent == 3 || (prevent == 1 && !cwin) || (prevent == 2 && cwin);
        if (clip) main = 0;
        int math = !nomath && ((ml == 4) ? (mo && (cgadsub & 0x10)) : (cgadsub >> (ml == 5 ? 5 : ml)) & 1);
        if (math) {
            int half = cgadsub & 0x40, subtract = cgadsub & 0x80;
            if (cgwsel & 2) {
                compose(s, l, s->ppu[0x2D], s->ppu[0x2F], x, ord, nord, &sc, &sl, &so);
                if (sl == 5) { sub = p->fixed; half = 0; } else sub = p->cgram[sc];
            } else sub = p->fixed;
            if (clip) half = 0;
            int out = 0;
            for (int sh = 0; sh < 15; sh += 5) {
                int a = (main >> sh) & 31, b = (sub >> sh) & 31;
                int v = subtract ? a - b : a + b;
                if (half) v >>= 1;
                if (v < 0) v = 0;
                if (v > 31) v = 31;
                out |= v << sh;
            }
            main = out;
        }
        row[x] = rgb(main, bright);
    }
}
