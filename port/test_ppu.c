/* Unit tests for the front-end PPU (port/ppu.c): pictures built register by
 * register, checked pixel by pixel. make ppu-test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes.h"
#include "ppu.h"

static Snes s;
static Ppu p;
static int fails;

static void wr(uint8_t r, uint8_t v) { s.ppu[r] = v; ppu_write(&s, r, v); }
static void wr16x2(uint8_t r, int v) { wr(r, (uint8_t)v); wr(r, (uint8_t)(v >> 8)); }   /* M7 latch: low, then high */
static void reset(void)
{
    memset(&s, 0, sizeof s);
    memset(&p, 0, sizeof p);
    s.ppu_state = &p;
    wr(0x00, 0x0F);   /* full brightness */
    p.cgram[1] = 0x001F;   /* red */
    p.cgram[2] = 0x03E0;   /* green */
}
static uint32_t at(int x, int y) { return p.pixels[(y - 1) * 256 + x] & 0xFFFFFF; }
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
enum { RED = 0xFF0000, GREEN = 0x00FF00, BLACK = 0 };

/* mode 7: map tile (tx,ty) = t; tile 1 all colour 1, tile 2 colour 2 */
static void m7_scene(void)
{
    for (int i = 0; i < 64; i++) { p.vram[64 + i] |= 1 << 8; p.vram[128 + i] |= 2 << 8; }
    p.vram[0 * 128 + 1] |= 1;      /* tile (1,0): red */
    p.vram[2 * 128 + 0] |= 2;      /* tile (0,2): green */
    wr(0x05, 0x07);
    wr(0x2C, 0x01);
    wr16x2(0x1B, 0x100); wr16x2(0x1C, 0); wr16x2(0x1D, 0); wr16x2(0x1E, 0x100);
    wr16x2(0x1F, 0); wr16x2(0x20, 0);
}

static void test_mode7_identity(void)
{
    reset();
    m7_scene();
    wr16x2(0x0D, 0); wr16x2(0x0E, 0);
    ppu_line(&s, 1);
    CHECK(at(8, 1) == RED && at(15, 1) == RED, "tile (1,0) at x 8..15: %06X %06X", at(8, 1), at(15, 1));
    CHECK(at(7, 1) == BLACK && at(16, 1) == BLACK, "around it: backdrop %06X %06X", at(7, 1), at(16, 1));
    ppu_line(&s, 17);
    CHECK(at(0, 17) == GREEN, "tile (0,2) at line 17: %06X", at(0, 17));
}

static void test_mode7_scroll(void)
{
    reset();
    m7_scene();
    wr16x2(0x0D, 8); wr16x2(0x0E, 16);   /* M7HOFS 8, M7VOFS 16: tile (1,2) row... (0,2) is at x -8 */
    ppu_line(&s, 1);
    CHECK(at(0, 1) == BLACK, "x 0 shows map (8,17): %06X", at(0, 1));
    wr16x2(0x0E, 0);
    ppu_line(&s, 1);
    CHECK(at(0, 1) == RED && at(8, 1) == BLACK, "HOFS 8: tile (1,0) at x 0..7: %06X %06X", at(0, 1), at(8, 1));
}

static void test_mode7_zoom(void)
{
    reset();
    m7_scene();
    wr16x2(0x1B, 0x200);   /* A = 2: two map pixels per screen pixel across */
    wr16x2(0x0D, 0); wr16x2(0x0E, 0);
    ppu_line(&s, 1);
    CHECK(at(4, 1) == RED && at(7, 1) == RED && at(8, 1) == BLACK && at(3, 1) == BLACK, "x 4..7 = map 8..15: %06X %06X %06X %06X",
          at(3, 1), at(4, 1), at(7, 1), at(8, 1));
}

static void test_mode7_outside_transparent(void)
{
    reset();
    m7_scene();
    p.vram[(1023 >> 3) * 128 + 0] |= 1;   /* tile (0,127): wraps to line -1 */
    wr16x2(0x0D, 0); wr16x2(0x0E, -2 & 0x1FFF);
    ppu_line(&s, 1);   /* map line -1 */
    CHECK(at(0, 1) == RED, "wrap: map line -1 is 1023: %06X", at(0, 1));
    wr(0x1A, 0x80);
    ppu_line(&s, 1);
    CHECK(at(0, 1) == BLACK, "M7SEL $80: outside is transparent: %06X", at(0, 1));
}

int main(void)
{
    test_mode7_identity();
    test_mode7_scroll();
    test_mode7_zoom();
    test_mode7_outside_transparent();
    printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
