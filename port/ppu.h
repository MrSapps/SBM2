/* PPU for the port's front-end: VRAM/CGRAM/OAM as the game writes them, and
 * a scanline renderer for what Super Bomberman 2 uses (BG modes 0 and 1,
 * 8x8/16x16 tiles, sprites, priorities, colour math, brightness). No mode 7,
 * mosaic or interlace. The verifier runs without one (Snes.ppu_state NULL);
 * PPU reads stay unmodelled either way. */
#ifndef PPU_H
#define PPU_H
#include <stdint.h>

typedef struct Ppu {
    uint16_t vram[0x8000];      /* words */
    uint16_t cgram[256];        /* BGR555 */
    uint8_t oam[544];
    uint16_t vaddr;             /* $2116/7, word address */
    uint8_t cgaddr, cglatch, cgflip;
    uint16_t oamaddr, oamreload; /* byte address; reload value from $2102/3 */
    uint8_t oamlatch;
    uint16_t hofs[4], vofs[4];  /* $210D-$2114, 10 bits */
    uint8_t ofslatch, hofslatch;
    uint16_t fixed;             /* $2132 fixed colour, BGR555 */
    uint32_t pixels[256 * 224]; /* XRGB8888, line 1 of the frame is row 0 */
} Ppu;

struct Snes;
/* A write to $21rr (rr < $40); the raw value is also in Snes.ppu[rr]. */
void ppu_write(struct Snes *s, uint8_t r, uint8_t v);
/* Start of vblank: OAM address reload. */
void ppu_vblank(struct Snes *s);
/* Draw visible line y (1..224) from the current registers. */
void ppu_line(struct Snes *s, int y);

#endif
