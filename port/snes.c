/* SNES bus: memory map, CPU-side registers, DMA/HDMA, timing, fake APU.
 *
 * Timing is modelled closely enough to reproduce Mesen's lag frames (the
 * game's logic sometimes overruns a frame, and which frames do depends on
 * a few dozen master clocks): every bus access costs its region's speed
 * (6/8/12 master clocks), every CPU internal cycle 6, a DMA byte 8; DRAM
 * refresh takes 40 clocks per line; HDMA is run for its cost. A line is 1364
 * clocks, a frame 262 lines (line 240 is 4 clocks short on odd fields),
 * vblank and NMI start at line 225. Per-instruction cycle counts match Mesen
 * exactly; the fitted constants are marked.
 *
 * APU: no SPC700. The ports emulate what this game's code waits for
 * (C3:A303 upload, C3:A436 command queue, C3:A385 re-upload), with delays
 * fitted to Mesen so loading screens take as many frames as there:
 *  - IPL mode: ports read $AA/$BB until $CC is written to $2140; then each
 *    write to $2140 is echoed. A write equal to the expected byte index is a
 *    data byte; any other value starts a block, or with $2141 = 0 jumps to the
 *    uploaded code (driver mode). Data is discarded.
 *  - Driver mode: a value written to $2140 or $2141 is acknowledged as
 *    value ^ $80 (C3:A436 checks this before sending the next command);
 *    $2142/$2143 read $AA (C3:A575/A5AE wait for it). Until the first write,
 *    $2140 keeps the IPL echo and $2141 still reads $BB, as the real driver
 *    has not touched them yet when C3:A303 samples them ($1C88 = $3B).
 *  - Back to IPL mode on $FF to $2140 (C3:A516) or $10 to $2141 (C3:A533;
 *    assumes no sound command is ever $10 on $2141).
 * Timings are in the "fake APU" section below.
 *
 * Frame-step mode (s->framestep) makes the game's state independent of time,
 * for running an interpreter and a partly native port side by side:
 *  - NMI is raised when the CPU reaches the cmp of an NMI wait loop (after its
 *    lda, so the loop exits right after the handler). Each such NMI is one
 *    logic frame; the pad is latched then from input[logic frame]. Or, with
 *    s->pads (a log of every pad read, bot/inputlog.lua), each read of $4219
 *    takes the next value: the game reads the pad in mainline code, and during
 *    a long loading screen in Mesen such a read sees a later frame's pad than
 *    the last NMI, which per-logic-frame input cannot reproduce.
 *  - Vblank no longer raises NMI or reads pads; HDMA is not run; V-IRQ off.
 *  - $4212: auto-joypad never busy, vblank/hblank bits toggle on every read
 *    (the game only waits on bit 0, but nothing can deadlock either way).
 *    $4210 bit 7 is set by each frame-step NMI.
 *  - APU: echoes, the driver switch, its $AA ready flag and acks are all
 *    immediate.
 * DMA still counts time, but nothing reads the clock in this mode. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes.h"

enum { LINE = 1364, LINES = 262, VBLANK_LINE = 225, REFRESH_AT = 538,
       REFRESH = 40, AUTOJOY_AT = 130, AUTOJOY_LEN = 4224,
       HDMA_INIT_AT = 12, HDMA_AT = 1104, NMI_H = 0 };

/* The CPU samples NMI before the last cycle of an instruction: an NMI raised
 * later is taken only after the next instruction (see snes_step). */
static void raise_nmi(Snes *s, uint64_t at)
{
    s->nmi_pending = 1;
    s->nmi_at = at;
}

static void line_start(Snes *s)
{
    s->vcount++;
    if (s->vcount == LINES) {
        s->vcount = 0;
        s->field ^= 1;
        if (!s->framestep) s->rdnmi &= 0x7F;
    }
    if (s->framestep) return;
    if (s->vcount == VBLANK_LINE) {
        s->frame++;
        s->rdnmi |= 0x80;
        if (s->nmitimen & 0x80) raise_nmi(s, s->clock - s->hclock + NMI_H);
        if (s->nmitimen & 0x01) {
            /* Auto-joypad read: pad 1 only; ports 2-4 have nothing plugged. */
            uint16_t pad = s->frame < s->input_len ? s->input[s->frame] : 0;
            s->joy[0] = pad;
            s->joy[1] = s->joy[2] = s->joy[3] = 0;
            s->ser[0] = 0xFFFFFFFFu;  /* bits consumed; a pad then reads 1s */
            s->ser[1] = 0;
            s->autojoy_end = s->clock + (AUTOJOY_AT - s->hclock) + AUTOJOY_LEN;
        }
    }
    /* V-IRQ only (H-IRQ unused by this game): fires at the start of line VTIME. */
    if ((s->nmitimen & 0x20) && s->vcount == s->vtime) {
        s->timeup = 0x80;
        s->cpu.irq = 1;
    }
}

static void hdma_init(Snes *s);
static void hdma_line(Snes *s);

/* HDMA and refresh add their cost straight to the clock (no recursion). */
static void tick(Snes *s, uint32_t n)
{
    s->cycle_start = s->clock;
    s->clock += n;
    s->hclock += n;
    for (;;) {
        if (s->framestep) {
            /* no HDMA */
        } else if (!s->hdma_inited && s->vcount == 0 && s->hclock >= HDMA_INIT_AT) {
            s->hdma_inited = 1;
            hdma_init(s);
        }
        if (!s->refreshed && s->hclock >= REFRESH_AT) {
            s->refreshed = 1;
            s->clock += REFRESH;
            s->hclock += REFRESH;
        }
        if (!s->framestep && !s->hdma_done && s->vcount < VBLANK_LINE && s->hclock >= HDMA_AT) {
            s->hdma_done = 1;
            hdma_line(s);
        }
        uint32_t len = (s->vcount == 240 && s->field) ? LINE - 4 : LINE;
        if (s->hclock < len) break;
        s->hclock -= len;
        s->refreshed = 0;
        s->hdma_done = 0;
        line_start(s);
        if (s->vcount == 0) s->hdma_inited = 0;
    }
}

/* Master clocks for an access to addr. */
static uint32_t speed(Snes *s, uint32_t addr)
{
    uint8_t b = addr >> 16;
    uint16_t a = addr;
    if (b & 0x40) {
        if ((b & 0xFE) == 0x7E) return 8;
        return (b & 0x80) && (s->memsel & 1) ? 6 : 8;
    }
    if (a & 0x8000) return (b & 0x80) && (s->memsel & 1) ? 6 : 8;
    if (a < 0x2000 || a >= 0x6000) return 8;
    if (a >= 0x4000 && a < 0x4200) return 12;
    return 6;
}

static uint8_t rom_byte(Snes *s, uint32_t addr)
{
    uint32_t o = addr & 0x3FFFFF;
    while (o >= s->rom_size) o -= s->rom_size > 0x200000 ? 0x200000 : s->rom_size;
    return s->rom[o];
}

/* --- fake APU ---
 * Echo timing of the IPL / loader, in SPC cycles (kept in 1/1000 master
 * clocks). The IPL data loop is
 *   cmp y,$F4 (3) / bne (2, 4 taken) / mov a,$F5 (3) / mov $F4,y (4) /
 *   mov [$00]+y,a (7) / inc y (2) / bne (4)
 * so it samples $F4 every 7 cycles (on the 3rd cycle of cmp), echoes 9
 * cycles after the sample that saw the new index and is back at cmp 16
 * cycles later; starting a block costs BLOCK more. The driver's own loader
 * (entered with $10 on $2141) is back after ~13 (fitted) and needs KICK cycles to
 * notice the $CC kick. An SPC cycle is nominally ~20.95 master clocks; 21.01
 * makes the 45k-byte boot upload end within ~1k clocks of Mesen (per block
 * it is off by up to 1%: the real time per byte depends on how the CPU and
 * SPC polls interleave).
 * The driver itself (fitted to Mesen, master clocks): it answers $10 after
 * SWITCH_IDLE, or SWITCH_BUSY once it has been sent a $2141 (music)
 * command; after a loader upload it raises $AA on $2142/$2143 INIT_IDLE /
 * INIT_BUSY clocks after the jump, the same way. It acknowledges a port
 * 0/1 write after ACK clocks (between 37k and 109k by Mesen's timeline;
 * the game only checks acks once per frame, except right after an upload). */
#define SPC_MC 21010ull
enum { BLOCK = 23, KICK = 517 };
enum { SWITCH_IDLE = 54000, SWITCH_BUSY = 189300,
       INIT_IDLE = 54000, INIT_BUSY = 65600, ACK = 60000 };

static void apu_ipl(Snes *s, int loader)
{
    memcpy(s->apu_prev, s->apu_out, 4);
    s->apu_switch_at = loader && !s->framestep ?
        s->clock + (s->apu_busy ? SWITCH_BUSY : SWITCH_IDLE) : 0;
    s->apu_loader = loader ? 1 + s->apu_busy : 0;
    s->apu_mode = 0;
    s->apu_echo_pending = 0;
    s->apu_ack_at[0] = s->apu_ack_at[1] = 0;
    s->apu_ready = s->apu_switch_at > s->clock ? s->apu_switch_at * 1000 : s->clock * 1000;
    s->apu_out[0] = 0xAA;
    s->apu_out[1] = 0xBB;
    s->apu_out[2] = s->apu_out[3] = 0;
}

static void apu_echo(Snes *s, uint8_t v, int block)
{
    uint64_t now = s->clock * 1000, t = s->apu_ready + 3 * SPC_MC;
    if (now > t) t += (now - t + 7 * SPC_MC - 1) / (7 * SPC_MC) * (7 * SPC_MC);
    t += (9 + (block ? BLOCK : 0)) * SPC_MC;
    if (s->apu_mode == 0 && s->apu_loader) t += KICK * SPC_MC;
    s->apu_echo_at = s->framestep ? 0 : t / 1000;
    s->apu_echo_val = v;
    s->apu_echo_pending = 1;
    s->apu_ready = t + (s->apu_loader ? 13 * SPC_MC - 550 : 16 * SPC_MC);
}

static void apu_start_driver(Snes *s)
{
    s->apu_mode = 2;
    s->apu_busy = 0;
    s->apu_aa_at = !s->apu_loader || s->framestep ? 0 :
        s->apu_echo_at + (s->apu_loader == 2 ? INIT_BUSY : INIT_IDLE);
}

static uint8_t apu_read(Snes *s, int port)
{
    if (s->clock < s->apu_switch_at) return s->apu_prev[port];
    if (s->apu_echo_pending && s->clock >= s->apu_echo_at) {
        s->apu_out[0] = s->apu_echo_val;
        s->apu_echo_pending = 0;
    }
    if (port < 2 && s->apu_ack_at[port] && s->clock >= s->apu_ack_at[port]) {
        s->apu_out[port] = s->apu_ack_val[port];
        s->apu_ack_at[port] = 0;
    }
    if (port >= 2 && s->apu_mode == 2)
        return s->clock >= s->apu_aa_at ? 0xAA : 0;
    return s->apu_out[port];
}

static void apu_write(Snes *s, int port, uint8_t v)
{
    s->apu_in[port] = v;
    switch (s->apu_mode) {
    case 0:     /* IPL waiting for the $CC kick */
        if (port != 0 || v != 0xCC) break;
        /* the kick is a block command */
        /* fall through */
    case 1:     /* IPL transfer */
        if (port != 0) break;
        if (s->apu_mode == 1 && v == s->apu_count) {
            apu_echo(s, v, 0);
            s->apu_count++;
            break;
        }
        apu_echo(s, v, 1);
        s->apu_count = 0;
        s->apu_mode = 1;
        if (s->apu_in[1] == 0) apu_start_driver(s);
        break;
    case 2:     /* driver */
        if (port == 0 && v == 0xFF) { apu_ipl(s, 0); break; }
        if (port == 1 && v == 0x10) { apu_ipl(s, 1); break; }
        if (port == 1) s->apu_busy = 1;
        if (port < 2) {
            if (port == 0) s->apu_echo_pending = 0;
            s->apu_ack_val[port] = v ^ 0x80;
            s->apu_ack_at[port] = s->clock + (s->framestep ? 0 : ACK);
        }
        break;
    }
}

/* --- DMA --- */
static uint8_t bbus_read(Snes *s, uint8_t r);
static void bbus_write(Snes *s, uint8_t r, uint8_t v);

static void run_dma(Snes *s, uint8_t mask)
{
    static const uint8_t pattern[8][4] = {
        {0,0,0,0}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1},
        {0,1,2,3}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1},
    };
    uint64_t start = s->clock;
    uint8_t cpu_speed = s->cpu_speed;
    tick(s, 8 - (s->clock & 7));
    for (int ch = 0; ch < 8; ch++) {
        if (!(mask & 1 << ch)) continue;
        uint8_t *r = s->dma[ch];
        uint8_t ctrl = r[0], bb = r[1];
        uint16_t aa = r[2] | r[3] << 8;
        uint8_t bank = r[4];
        uint32_t n = r[5] | r[6] << 8;
        if (!n) n = 0x10000;
        tick(s, 8);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t breg = bb + pattern[ctrl & 7][i & 3];
            uint32_t a = (uint32_t)bank << 16 | aa;
            /* A-bus can't reach B-bus registers or WRAM <-> $2180. */
            uint16_t lo = aa;
            int a_io = !(bank & 0x40) && ((lo >= 0x2100 && lo < 0x2200) ||
                                          (lo >= 0x4000 && lo < 0x4400));
            int a_wram = (bank & 0xFE) == 0x7E || (!(bank & 0x40) && lo < 0x2000);
            if (ctrl & 0x80) {        /* B -> A */
                uint8_t v = bbus_read(s, breg);
                if (!a_io && !(a_wram && breg == 0x80)) {
                    uint8_t save = s->mdr;
                    s->cpu.write(s, a, v);
                    s->mdr = save;
                }
            } else {                  /* A -> B */
                uint8_t v = a_io ? s->mdr : s->cpu.read(s, a);
                if (!(a_wram && breg == 0x80)) bbus_write(s, breg, v);
            }
            /* cpu.read/write ticked already; DMA is 8 clocks per byte */
            if (!(ctrl & 0x08)) aa += (ctrl & 0x10) ? -1 : 1;
        }
        r[2] = aa; r[3] = aa >> 8;
        r[5] = r[6] = 0;
    }
    tick(s, cpu_speed - (s->clock - start) % cpu_speed);
}

/* --- B-bus ($21xx) --- */
static uint8_t bbus_read(Snes *s, uint8_t r)
{
    if (r >= 0x40 && r < 0x80) return apu_read(s, r & 3);
    if (r == 0x80) {
        uint8_t v = s->wram[s->wram_addr];
        s->wram_addr = (s->wram_addr + 1) & 0x1FFFF;
        return v;
    }
    return s->mdr;  /* PPU reads are not modelled */
}

static void bbus_write(Snes *s, uint8_t r, uint8_t v)
{
    if (r < 0x40) { s->ppu[r] = v; return; }
    if (r < 0x80) { apu_write(s, r & 3, v); return; }
    switch (r) {
    case 0x80: s->wram[s->wram_addr] = v; s->wram_addr = (s->wram_addr + 1) & 0x1FFFF; break;
    case 0x81: s->wram_addr = (s->wram_addr & 0x1FF00) | v; break;
    case 0x82: s->wram_addr = (s->wram_addr & 0x100FF) | v << 8; break;
    case 0x83: s->wram_addr = (s->wram_addr & 0x0FFFF) | (v & 1) << 16; break;
    }
}

/* --- HDMA: run for its cost (and B-bus writes): 8 clocks per line, 8 per active
 * channel, 8 per byte moved or table byte read, 16 per indirect address
 * load. Like Mesen, (H)DMA starts on an 8-clock boundary and the CPU then
 * resumes on a boundary of its current cycle length (dma_cost). --- */
static uint32_t dma_cost(Snes *s, uint32_t cost)
{
    /* The +6 phase is fitted: it makes HDMA cost per frame match Mesen's
     * to within ~50 clocks (8-clock alignment on our raw clock is ~250 off). */
    uint32_t c = 8 - ((s->clock + 6) & 7) + cost;
    return c + s->cpu_speed - c % s->cpu_speed;
}

static const uint8_t hdma_len[8] = {1, 2, 2, 4, 4, 4, 2, 4};
static const uint8_t hdma_pat[8][4] = {
    {0,0,0,0}, {0,1,0,0}, {0,0,0,0}, {0,0,1,1},
    {0,1,2,3}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1},
};

static uint8_t hdma_table_byte(Snes *s, int ch)
{
    uint8_t *r = s->dma[ch];
    uint16_t a = r[8] | r[9] << 8;
    uint8_t v = snes_peek(s, (uint32_t)r[4] << 16 | a);
    a++;
    r[8] = a; r[9] = a >> 8;
    return v;
}

/* Loads the next line counter (and indirect address); returns its cost. */
static uint32_t hdma_next_entry(Snes *s, int ch)
{
    uint8_t *r = s->dma[ch];
    uint32_t cost = 8;
    r[0xA] = hdma_table_byte(s, ch);
    if (r[0] & 0x40) {
        r[5] = hdma_table_byte(s, ch);
        r[6] = hdma_table_byte(s, ch);
        cost += 16;
    }
    if (!r[0xA]) s->hdma_live &= ~(1 << ch);
    s->hdma_xfer |= 1 << ch;
    return cost;
}

static void hdma_init(Snes *s)
{
    s->hdma_live = s->hdmaen;
    s->hdma_xfer = 0;
    if (!s->hdmaen) return;
    uint32_t cost = 0;
    for (int ch = 0; ch < 8; ch++) {
        if (!(s->hdmaen & 1 << ch)) continue;
        uint8_t *r = s->dma[ch];
        r[8] = r[2]; r[9] = r[3];
        cost += hdma_next_entry(s, ch);
    }
    cost = dma_cost(s, cost);
    s->clock += cost;
    s->hclock += cost;
}

static void hdma_line(Snes *s)
{
    uint8_t live = s->hdma_live & s->hdmaen;
    if (!live) return;
    uint32_t cost = 8;
    for (int ch = 0; ch < 8; ch++) {
        if (!(live & 1 << ch)) continue;
        uint8_t *r = s->dma[ch];
        uint8_t mode = r[0] & 7;
        cost += 8;
        if (s->hdma_xfer & 1 << ch) {
            for (int i = 0; i < hdma_len[mode]; i++) {
                uint32_t a;
                if (r[0] & 0x40) {
                    uint16_t ia = r[5] | r[6] << 8;
                    a = (uint32_t)r[7] << 16 | ia++;
                    r[5] = ia; r[6] = ia >> 8;
                } else {
                    a = (uint32_t)r[4] << 16 | (r[8] | r[9] << 8);
                    hdma_table_byte(s, ch);
                }
                if (!(r[0] & 0x80)) bbus_write(s, r[1] + hdma_pat[mode][i], snes_peek(s, a));
                cost += 8;
            }
        }
        r[0xA]--;
        if (r[0xA] & 0x80) s->hdma_xfer |= 1 << ch;
        else s->hdma_xfer &= ~(1 << ch);
        if (!(r[0xA] & 0x7F)) cost += hdma_next_entry(s, ch);
    }
    cost = dma_cost(s, cost);
    s->clock += cost;
    s->hclock += cost;
}

/* --- CPU registers $4016-$4017, $4200-$43FF --- */
static uint8_t reg_read(Snes *s, uint16_t a)
{
    uint8_t v;
    switch (a) {
    case 0x4016: v = (s->mdr & 0xFC) | (s->ser[0] & 1); s->ser[0] = s->ser[0] >> 1 | 0x80000000u; return v;
    case 0x4017: return (s->mdr & 0xE0) | 0x1C;   /* port 2 empty */
    case 0x4210: v = (s->mdr & 0x70) | s->rdnmi | 0x02; s->rdnmi &= 0x7F; return v;
    case 0x4211: v = (s->mdr & 0x7F) | s->timeup; s->timeup = 0; s->cpu.irq = 0; return v;
    case 0x4212:
        if (s->framestep) return (s->mdr & 0x3E) | (s->fs_toggle ^= 0xC0);
        return (s->mdr & 0x3E) |
               (s->vcount >= VBLANK_LINE ? 0x80 : 0) |
               (s->hclock >= 1096 || s->hclock < 4 ? 0x40 : 0) |
               (s->clock < s->autojoy_end ? 0x01 : 0);
    case 0x4213: return s->wrio;
    case 0x4214: return s->rddiv;
    case 0x4215: return s->rddiv >> 8;
    case 0x4216: return s->rdmpy;
    case 0x4217: return s->rdmpy >> 8;
    }
    if (a == 0x4219 && (s->pads || s->pad_record)) {
        /* A pad read (the game reads $4219, then $4218): replay or record. */
        if (s->pads) {
            while (s->pad_pos < s->npads && (s->pads[s->pad_pos] & 0xF)) {
                s->pad_pos++;   /* an NMI event that frame-step could not place */
                s->pad_desync++;
            }
            s->joy[0] = s->pad_pos < s->npads ? s->pads[s->pad_pos] : 0;
        }
        s->pad_pos++;
        if (s->pad_record) {
            if (s->pad_rec_len == s->pad_rec_cap) {
                s->pad_rec_cap = s->pad_rec_cap ? 2 * s->pad_rec_cap : 4096;
                s->pad_rec = realloc(s->pad_rec, s->pad_rec_cap * 2);
            }
            s->pad_rec[s->pad_rec_len++] = s->joy[0];
        }
    }
    if (a >= 0x4218 && a < 0x4220) {
        uint16_t j = s->joy[(a - 0x4218) >> 1];
        return a & 1 ? j >> 8 : j;
    }
    if (a >= 0x4300 && a < 0x4380) {
        uint8_t r = a & 0xF;
        return r < 0xC || r == 0xF ? s->dma[(a >> 4) & 7][r == 0xF ? 0xB : r] : s->mdr;
    }
    return s->mdr;
}

static void reg_write(Snes *s, uint16_t a, uint8_t v)
{
    switch (a) {
    case 0x4016:
        if ((s->strobe & 1) && !(v & 1)) {     /* latch on 1 -> 0 */
            uint16_t pad = s->frame < s->input_len ? s->input[s->frame] : 0;
            s->ser[0] = 0xFFFF0000u;
            for (int i = 0; i < 16; i++) s->ser[0] |= (uint32_t)(pad >> (15 - i) & 1) << i;
            s->ser[1] = 0;
        }
        s->strobe = v;
        return;
    case 0x4200:
        if ((v & 0x80) && !(s->nmitimen & 0x80) && (s->rdnmi & 0x80)) raise_nmi(s, s->clock);
        s->nmitimen = v;
        if (!(v & 0x30)) { s->cpu.irq = 0; s->timeup = 0; }
        return;
    case 0x4201: s->wrio = v; return;
    case 0x4202: s->wrmpya = v; return;
    case 0x4203: s->rdmpy = s->wrmpya * v; s->rddiv = v; return;
    case 0x4204: s->wrdiv = (s->wrdiv & 0xFF00) | v; return;
    case 0x4205: s->wrdiv = (s->wrdiv & 0x00FF) | v << 8; return;
    case 0x4206:
        if (v) { s->rddiv = s->wrdiv / v; s->rdmpy = s->wrdiv % v; }
        else { s->rddiv = 0xFFFF; s->rdmpy = s->wrdiv; }
        return;
    case 0x4207: s->htime = (s->htime & 0x100) | v; return;
    case 0x4208: s->htime = (s->htime & 0xFF) | (v & 1) << 8; return;
    case 0x4209: s->vtime = (s->vtime & 0x100) | v; return;
    case 0x420A: s->vtime = (s->vtime & 0xFF) | (v & 1) << 8; return;
    case 0x420B: if (v) run_dma(s, v); return;
    case 0x420C: s->hdmaen = v; return;
    case 0x420D: s->memsel = v; return;
    }
    if (a >= 0x4300 && a < 0x4380) {
        uint8_t r = a & 0xF;
        if (r < 0xC || r == 0xF) s->dma[(a >> 4) & 7][r == 0xF ? 0xB : r] = v;
    }
}

/* --- memory map --- */
static uint8_t bus_read(void *ctx, uint32_t addr)
{
    Snes *s = ctx;
    uint8_t b = addr >> 16;
    uint16_t a = addr;
    uint8_t v;
    tick(s, s->cpu_speed = speed(s, addr));
    if ((b & 0xFE) == 0x7E) v = s->wram[addr & 0x1FFFF];
    else if (b & 0x40) v = rom_byte(s, addr);
    else if (a & 0x8000) v = rom_byte(s, addr);
    else if (a < 0x2000) v = s->wram[a];
    else if (a >= 0x2100 && a < 0x2200) v = bbus_read(s, a);
    else if (a == 0x4016 || a == 0x4017 || (a >= 0x4200 && a < 0x4400)) v = reg_read(s, a);
    else if (a >= 0x6000 && s->sram_size && (b & 0x7F) >= 0x20)
        v = s->sram[(((b & 0x1F) << 13) | (a & 0x1FFF)) & (s->sram_size - 1)];
    else v = s->mdr;
    return s->mdr = v;
}

static void bus_write(void *ctx, uint32_t addr, uint8_t v)
{
    Snes *s = ctx;
    uint8_t b = addr >> 16;
    uint16_t a = addr;
    tick(s, s->cpu_speed = speed(s, addr));
    s->mdr = v;
    if ((b & 0xFE) == 0x7E) s->wram[addr & 0x1FFFF] = v;
    else if ((b & 0x40) || (a & 0x8000)) return;
    else if (a < 0x2000) s->wram[a] = v;
    else if (a >= 0x2100 && a < 0x2200) bbus_write(s, a, v);
    else if (a == 0x4016 || (a >= 0x4200 && a < 0x4400)) reg_write(s, a, v);
    else if (a >= 0x6000 && s->sram_size && (b & 0x7F) >= 0x20)
        s->sram[(((b & 0x1F) << 13) | (a & 0x1FFF)) & (s->sram_size - 1)] = v;
}

static void bus_idle(void *ctx) { Snes *s = ctx; tick(s, s->cpu_speed = 6); }

uint8_t snes_peek(void *ctx, uint32_t addr)
{
    Snes *s = ctx;
    uint8_t b = addr >> 16;
    uint16_t a = addr;
    if ((b & 0xFE) == 0x7E) return s->wram[addr & 0x1FFFF];
    if ((b & 0x40) || (a & 0x8000)) return rom_byte(s, addr);
    if (a < 0x2000) return s->wram[a];
    return 0;
}

uint8_t snes_read(Snes *s, uint32_t addr) { return bus_read(s, addr & 0xFFFFFF); }
void snes_write(Snes *s, uint32_t addr, uint8_t v) { bus_write(s, addr & 0xFFFFFF, v); }

static void find_wait_loops(Snes *s)
{
    static const uint8_t pat[8] = {0xAD, 0x36, 0x00, 0xCD, 0x36, 0x00, 0xF0, 0xFB};
    s->wait_at = calloc(0x10000, 1);
    for (uint32_t o = 0; o + 8 <= s->rom_size && s->nwait < 62; o++) {
        if (memcmp(s->rom + o, pat, 8)) continue;
        s->wait_lo[s->nwait++] = 0xC00000 + o;
        /* code in $8000-$FFFF can also run from the bank $00-$3F mirror */
        if ((o & 0xFFFF) >= 0x8000 && o < 0x400000) s->wait_lo[s->nwait++] = o;
        s->wait_at[o & 0xFFFF] |= WAIT_LDA;
        s->wait_at[(o + 3) & 0xFFFF] |= WAIT_CMP;
        s->wait_at[(o + 6) & 0xFFFF] |= WAIT_BEQ;
        s->wait_at[(o + 8) & 0xFFFF] |= WAIT_EXIT;
    }
}

int snes_wait_point(const Snes *s, uint32_t pc)
{
    int k = s->wait_at[pc & 0xFFFF];
    for (int i = 0; k && i < s->nwait; i++) {
        if ((k & WAIT_LDA) && s->wait_lo[i] == pc) return WAIT_LDA;
        if ((k & WAIT_CMP) && s->wait_lo[i] == pc - 3) return WAIT_CMP;
        if ((k & WAIT_BEQ) && s->wait_lo[i] == pc - 6) return WAIT_BEQ;
        if ((k & WAIT_EXIT) && s->wait_lo[i] == pc - 8) return WAIT_EXIT;
    }
    return 0;
}

uint16_t *snes_load_input(const char *path, uint32_t *len)
{
    static const struct { const char *name; uint16_t bit; } names[] = {
        {"a", PAD_A}, {"b", PAD_B}, {"x", PAD_X}, {"y", PAD_Y}, {"l", PAD_L},
        {"r", PAD_R}, {"start", PAD_START}, {"select", PAD_SELECT},
        {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
    };
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    uint32_t cap = 1024, n = 0, frame;
    uint16_t *in = calloc(cap, 2), held = 0;
    char line[256], btn[200];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%u %199s", &frame, btn) != 2) continue;
        while (n < frame) {
            if (n == cap) { cap *= 2; in = realloc(in, cap * 2); }
            in[n++] = held;
        }
        if (!strcmp(btn, "end")) break;
        held = 0;
        for (char *t = strtok(btn, "+"); t; t = strtok(NULL, "+"))
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (!strcmp(t, names[i].name)) held |= names[i].bit;
    }
    fclose(f);
    *len = n;
    return in;
}

int snes_load(Snes *s, const char *path)
{
    memset(s, 0, sizeof *s);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long skip = size % 1024 == 512 ? 512 : 0;
    fseek(f, skip, SEEK_SET);
    size -= skip;
    if (size <= 0x10000) { fclose(f); return -1; }
    s->rom = malloc(size);
    s->rom_size = size;
    if (fread(s->rom, 1, size, f) != (size_t)size) { fclose(f); return -1; }
    fclose(f);
    /* HiROM header at $FFC0; $FFD8 = SRAM size as 1 KiB << n. */
    uint8_t n = s->rom[0xFFD8];
    if (n && n <= 7) {
        s->sram_size = 0x400u << n;
        s->sram = calloc(1, s->sram_size);
    }
    find_wait_loops(s);
    return 0;
}

void snes_free(Snes *s)
{
    free(s->rom);
    free(s->sram);
    free(s->wait_at);
    free(s->pad_rec);
    s->rom = s->sram = s->wait_at = NULL;
    s->pad_rec = NULL;
}

void snes_power(Snes *s)
{
    memset(s->wram, 0, sizeof s->wram);
    memset(s->ppu, 0, sizeof s->ppu);
    memset(s->dma, 0xFF, sizeof s->dma);
    s->vcount = 0;
    s->field = 0;
    s->refreshed = 0;
    s->frame = 0;
    s->autojoy_end = 0;
    s->nmitimen = s->rdnmi = s->timeup = s->memsel = 0;
    s->wrio = 0xFF;
    s->wrmpya = 0xFF;
    s->wrdiv = 0xFFFF;
    s->rddiv = s->rdmpy = 0;
    s->htime = s->vtime = 0x1FF;
    memset(s->joy, 0, sizeof s->joy);
    s->ser[0] = s->ser[1] = 0;
    s->strobe = 0;
    s->wram_addr = 0;
    s->hdmaen = s->hdma_live = s->hdma_xfer = 0;
    s->hdma_inited = s->hdma_done = 0;
    s->mdr = 0;
    s->cpu_speed = 8;
    s->nmi_pending = 0;
    s->nmi_at = s->cycle_start = 0;
    s->fs_raised = s->fs_toggle = s->fs_seen16 = s->fs_in_nmi = 0;
    s->pad_desync = 0;
    s->pad_pos = s->pad_rec_len = 0;
    memset(s->apu_in, 0, sizeof s->apu_in);
    apu_ipl(s, 0);
    s->apu_count = s->apu_busy = 0;
    s->apu_switch_at = s->apu_aa_at = 0;

    Cpu *c = &s->cpu;
    memset(c, 0, sizeof *c);
    c->ctx = s;
    c->read = bus_read;
    c->write = bus_write;
    c->idle = bus_idle;
    c->sp = 0x1FF;
    cpu_reset(c);
    s->clock = s->hclock = 186;  /* where Mesen is at the first opcode fetch */
    s->refreshed = 0;
}

static uint16_t wram16(const Snes *s, uint16_t a) { return s->wram[a] | s->wram[a + 1] << 8; }

static void fs_raise(Snes *s)
{
    s->rdnmi |= 0x80;
    s->fs_in_nmi = 1;
    s->fs_nmi_sp = s->cpu.sp;
    s->cpu.nmi = 1;
}

/* Frame-step NMIs. Without NMI events in the pad log: one at each wait
 * loop's cmp. With them (a log of a real run), the log also says where the
 * real machine had NMIs that did not end a wait: one that hit a wait loop's
 * lda (the logic finished just at vblank) is raised there too, and makes the
 * loop wait for the next; others (lag, loading screens) at the first point
 * where $16 and $18 are as that NMI saw them - for one with $16 set, only
 * after $16 has been 0 in this logic frame (it came after the frame's logic)
 * - or at the latest at the wait loop's lda. Never inside a handler. */
static void framestep_nmi(Snes *s)
{
    uint32_t pc = (uint32_t)s->cpu.pb << 16 | s->cpu.pc;
    int k = s->wait_at[pc & 0xFFFF] ? snes_wait_point(s, pc) : 0;
    if (k == WAIT_EXIT) {   /* the frame boundary itself: nothing raised here */
        s->fs_raised = 0;
        s->fs_seen16 = 0;
        return;
    }
    if (s->fs_in_nmi) {
        if (s->cpu.sp < s->fs_nmi_sp) return;
        s->fs_in_nmi = 0;
    }
    if (!(s->nmitimen & 0x80)) return;
    if (s->pad_markers) {
        if (s->fs_raised) return;   /* the next frame's NMIs come after the exit */
        if (!wram16(s, 0x16)) s->fs_seen16 = 1;
        if (s->pad_pos < s->npads && (s->pads[s->pad_pos] & PADLOG_EXTRA)) {
            uint16_t v = s->pads[s->pad_pos];
            int set16 = wram16(s, 0x16) != 0, set18 = wram16(s, 0x18) != 0;
            int ok = !(v & PADLOG_AT_LDA) &&
                     set18 == !!(v & PADLOG_18_SET) && set16 == !!(v & PADLOG_16_SET) &&
                     (!set16 || s->fs_seen16);
            if (ok || k == WAIT_LDA) {
                s->pad_pos++;
                fs_raise(s);
            }
            return;
        }
        if (k != WAIT_CMP || s->fs_raised) return;
        if (s->pad_pos < s->npads && s->pads[s->pad_pos] == PADLOG_FRAME) s->pad_pos++;
        else s->pad_desync++;
    } else if (k != WAIT_CMP || s->fs_raised) return;
    s->fs_raised = 1;
    if (s->nmitimen & 0x01) {
        if (!s->pads) s->joy[0] = s->frame < s->input_len ? s->input[s->frame] : 0;
        s->joy[1] = s->joy[2] = s->joy[3] = 0;
        s->ser[0] = 0xFFFFFFFFu;
        s->ser[1] = 0;
    }
    s->frame++;
    fs_raise(s);
}

/* Timed mode, recording a pad log: note each NMI as the real machine has it. */
static void record_nmi(Snes *s)
{
    uint32_t pc = (uint32_t)s->cpu.pb << 16 | s->cpu.pc;
    int k = s->wait_at[pc & 0xFFFF] ? snes_wait_point(s, pc) : 0;
    uint16_t v = k == WAIT_CMP || k == WAIT_BEQ ? PADLOG_FRAME :
        PADLOG_EXTRA | (wram16(s, 0x16) ? PADLOG_16_SET : 0) | (wram16(s, 0x18) ? PADLOG_18_SET : 0) |
        (k == WAIT_LDA ? PADLOG_AT_LDA : 0);
    if (s->pad_rec_len == s->pad_rec_cap) {
        s->pad_rec_cap = s->pad_rec_cap ? 2 * s->pad_rec_cap : 4096;
        s->pad_rec = realloc(s->pad_rec, s->pad_rec_cap * 2);
    }
    s->pad_rec[s->pad_rec_len++] = v;
}

void snes_step(Snes *s)
{
    if (s->framestep) framestep_nmi(s);
    else if (s->nmi_pending && s->nmi_at <= s->cycle_start) {
        s->nmi_pending = 0;
        s->cpu.nmi = 1;
        if (s->pad_record) record_nmi(s);
    }
    if (s->hook && !s->cpu.nmi && !s->cpu.wai && !s->cpu.stp && s->hook(s)) return;
    cpu_step(&s->cpu);
}
