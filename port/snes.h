/* SNES bus for the port's verification runtime: HiROM cartridge, WRAM, the
 * CPU-side registers that affect game logic, DMA, and a coarse video timing
 * model (master clocks, scanlines, vblank, NMI, auto-joypad). Rendering only
 * with a Ppu attached (ppu.h); no SPC700: the APU ports are faked (snes.c). */
#ifndef SNES_H
#define SNES_H
#include <stdint.h>
#include "cpu.h"

/* Joypad bits as read from $4218/$4219. */
enum { PAD_R = 0x0010, PAD_L = 0x0020, PAD_X = 0x0040, PAD_A = 0x0080,
       PAD_RIGHT = 0x0100, PAD_LEFT = 0x0200, PAD_DOWN = 0x0400,
       PAD_UP = 0x0800, PAD_START = 0x1000, PAD_SELECT = 0x2000,
       PAD_Y = 0x4000, PAD_B = 0x8000 };

typedef struct Snes {
    Cpu cpu;
    uint8_t *rom;
    uint32_t rom_size;
    uint8_t *sram;
    uint32_t sram_size;
    uint8_t wram[0x20000];
    uint8_t ppu[0x40];          /* last value written to $2100-$213F */
    struct Ppu *ppu_state;      /* front-end only (ppu.h); NULL in the verifier */
    uint8_t mdr;                /* open bus */

    /* timing */
    uint64_t clock;             /* master clocks since power-on */
    uint32_t hclock;            /* position in the scanline */
    uint16_t vcount;
    uint8_t field;
    uint8_t refreshed;
    uint8_t cpu_speed;          /* clocks of the last CPU cycle */
    uint64_t cycle_start;       /* clock when the last CPU cycle began */
    uint64_t nmi_at;            /* when the pending NMI was raised */
    uint8_t nmi_pending;
    uint32_t frame;             /* vblanks started so far */
    uint64_t autojoy_end;       /* $4212 bit 0 is set until this clock */

    /* CPU registers $4200-$421F */
    uint8_t nmitimen, rdnmi, timeup, memsel, wrio;
    uint8_t wrmpya;
    uint16_t wrdiv, rddiv, rdmpy;
    uint16_t htime, vtime;
    uint16_t joy[4];            /* $4218-$421F */
    uint32_t ser[2];            /* $4016/$4017 serial shift registers */
    uint8_t strobe;

    uint32_t wram_addr;         /* $2181-$2183 */
    uint8_t dma[8][16];         /* $43x0-$43xF */
    uint8_t hdmaen, hdma_live, hdma_xfer;   /* channel bitmasks */
    uint8_t hdma_inited, hdma_done;

    /* fake APU, see snes.c */
    uint8_t apu_in[4], apu_out[4];
    uint8_t apu_mode, apu_count, apu_loader;
    uint8_t apu_echo_val, apu_echo_pending;
    uint8_t apu_busy;
    uint64_t apu_echo_at, apu_ready, apu_switch_at, apu_aa_at;
    uint8_t apu_prev[4], apu_ack_val[2];
    uint64_t apu_ack_at[2];

    /* input: pad 1 for frame n is input[n] (0 past the end); a frame is a
     * video frame, or a logic frame in frame-step mode */
    const uint16_t *input;
    uint32_t input_len;
    /* pad-read log: if pads is set, each read of $4219 takes the next value;
     * with pad_record, every read's value is appended to pad_rec */
    const uint16_t *pads;
    uint32_t npads, pad_pos;
    uint16_t *pad_rec;
    uint32_t pad_rec_len, pad_rec_cap;
    uint8_t pad_record;

    /* NMI wait loops (lda $0036 / cmp $0036 / beq), found in the ROM:
     * wait_at[pc & $FFFF] has a WAIT_* bit for each loop point there. */
    uint32_t wait_lo[64];
    int nwait;
    uint8_t *wait_at;

    /* Frame-step mode (see snes.c): time no longer drives NMI. */
    uint8_t framestep, fs_raised, fs_toggle;
    uint8_t fs_seen16, fs_in_nmi;   /* pad-log NMI events, see snes.c */
    uint16_t fs_nmi_sp;
    uint8_t pad_markers;            /* the pad log has NMI event markers */
    uint8_t fs_hold;                /* no frame-step NMIs (hook shadow check) */
    uint32_t pad_desync;            /* log entries that did not fit */

    /* Called before each instruction (not before taking an interrupt); if it
     * returns nonzero it has executed in place of the interpreter. */
    int (*hook)(struct Snes *s);
} Snes;

enum { WAIT_CMP = 1, WAIT_EXIT = 2, WAIT_LDA = 4, WAIT_BEQ = 8 };
/* Pad-log entries with a nonzero low nibble (never set in a pad word) are NMI
 * events: PADLOG_FRAME = the NMI that ended a wait (one logic frame), or
 * PADLOG_EXTRA | 16_SET | 18_SET [| AT_LDA] = another NMI, with the handler's
 * $16/$18 [and having hit a wait loop's lda]; or PADLOG_POKE | bank << 4,
 * offset, value = a 16-bit WRAM write by the recording script (3 words). */
enum { PADLOG_FRAME = 1, PADLOG_EXTRA = 2, PADLOG_POKE = 3, PADLOG_16_SET = 4,
       PADLOG_18_SET = 8, PADLOG_AT_LDA = 0x10 };

/* Loads the ROM (a copier header is skipped). Returns 0 on success. */
int snes_load(Snes *s, const char *path);
/* Input script ("<frame> <buttons>" lines, as bot/harness.lua) as one pad
 * word per frame; NULL if unreadable. */
uint16_t *snes_load_input(const char *path, uint32_t *len);
void snes_free(Snes *s);
/* Power-on: WRAM zeroed, registers reset, CPU reset. */
void snes_power(Snes *s);
/* One CPU instruction (or interrupt entry / idle cycle). */
void snes_step(Snes *s);
/* Debug view of the bus: no side effects, no time. */
uint8_t snes_peek(void *s, uint32_t addr);
/* Bus access as the CPU would make it (side effects and time). */
uint8_t snes_read(Snes *s, uint32_t addr);
void snes_write(Snes *s, uint32_t addr, uint8_t v);
/* WAIT_LDA / WAIT_CMP / WAIT_BEQ / WAIT_EXIT if pc (24-bit) is that point of
 * a wait loop. */
int snes_wait_point(const Snes *s, uint32_t pc);

#endif
