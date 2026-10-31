/* WDC 65816 interpreter: all 256 opcodes, native and emulation mode, decimal
 * ADC/SBC, BRK/COP, WAI/STP, MVN/MVP, NMI/IRQ. Internal operation cycles are
 * reported through idle() roughly where the real chip spends them. */
#include <stdio.h>
#include "cpu.h"

#define M8 (c->p & FM)
#define X8 (c->p & FX)

static uint8_t rd(Cpu *c, uint32_t a) { return c->read(c->ctx, a & 0xFFFFFF); }
static void wr(Cpu *c, uint32_t a, uint8_t v) { c->write(c->ctx, a & 0xFFFFFF, v); }
static void io(Cpu *c) { c->idle(c->ctx); }

static uint8_t fetch(Cpu *c) { return rd(c, (uint32_t)c->pb << 16 | c->pc++); }
static uint16_t fetch16(Cpu *c) { uint16_t lo = fetch(c); return lo | fetch(c) << 8; }
static uint32_t fetch24(Cpu *c) { uint32_t lo = fetch16(c); return lo | (uint32_t)fetch(c) << 16; }

static void push8(Cpu *c, uint8_t v)
{
    wr(c, c->sp, v);
    c->sp = c->e ? (0x100 | ((c->sp - 1) & 0xFF)) : (uint16_t)(c->sp - 1);
}
static uint8_t pull8(Cpu *c)
{
    c->sp = c->e ? (0x100 | ((c->sp + 1) & 0xFF)) : (uint16_t)(c->sp + 1);
    return rd(c, c->sp);
}
static void push16(Cpu *c, uint16_t v) { push8(c, v >> 8); push8(c, v); }
static uint16_t pull16(Cpu *c) { uint16_t lo = pull8(c); return lo | pull8(c) << 8; }

static void nz8(Cpu *c, uint8_t v) { c->p = (c->p & ~(FN | FZ)) | (v & FN) | (v ? 0 : FZ); }
static void nz16(Cpu *c, uint16_t v) { c->p = (c->p & ~(FN | FZ)) | (v >> 8 & FN) | (v ? 0 : FZ); }

/* Applies the side effects of writing P (and of E). */
static void fix_p(Cpu *c)
{
    if (c->e) c->p |= FM | FX;
    if (c->p & FX) { c->x &= 0xFF; c->y &= 0xFF; }
}

/* --- addressing: effective address, and whether a 16-bit access wraps in
 * bank 0 (direct page / stack) or runs on linearly --- */
typedef struct { uint32_t a; int w; } Ea;

static void dp_io(Cpu *c) { if (c->d & 0xFF) io(c); }
static Ea lin(uint32_t a) { Ea e = { a & 0xFFFFFF, 0 }; return e; }
static Ea bank0(uint32_t a) { Ea e = { a & 0xFFFF, 1 }; return e; }

static Ea am_dp(Cpu *c) { uint8_t o = fetch(c); dp_io(c); return bank0(c->d + o); }
static Ea am_dpi(Cpu *c, uint16_t i)
{
    uint8_t o = fetch(c);
    dp_io(c); io(c);
    if (c->e && !(c->d & 0xFF)) return bank0(c->d | ((o + i) & 0xFF));
    return bank0(c->d + o + i);
}
static uint16_t ptr16(Cpu *c, uint16_t p) { uint16_t lo = rd(c, p); return lo | rd(c, (uint16_t)(p + 1)) << 8; }
static uint32_t ptr24(Cpu *c, uint16_t p) { uint32_t lo = ptr16(c, p); return lo | (uint32_t)rd(c, (uint16_t)(p + 2)) << 16; }
static Ea am_dpind(Cpu *c) { Ea p = am_dp(c); return lin((uint32_t)c->db << 16 | ptr16(c, p.a)); }
static Ea am_dpindx(Cpu *c) { Ea p = am_dpi(c, c->x); return lin((uint32_t)c->db << 16 | ptr16(c, p.a)); }
static Ea am_dpindy(Cpu *c, int w)
{
    Ea p = am_dp(c);
    uint32_t base = (uint32_t)c->db << 16 | ptr16(c, p.a);
    uint32_t a = base + c->y;
    if (w || !X8 || ((base ^ a) & 0xFF00)) io(c);
    return lin(a);
}
static Ea am_dpindl(Cpu *c) { Ea p = am_dp(c); return lin(ptr24(c, p.a)); }
static Ea am_dpindly(Cpu *c) { Ea p = am_dp(c); return lin(ptr24(c, p.a) + c->y); }
static Ea am_sr(Cpu *c) { uint8_t o = fetch(c); io(c); return bank0(c->sp + o); }
static Ea am_srindy(Cpu *c)
{
    Ea p = am_sr(c);
    uint32_t base = (uint32_t)c->db << 16 | ptr16(c, p.a);
    io(c);
    return lin(base + c->y);
}
static Ea am_abs(Cpu *c) { return lin((uint32_t)c->db << 16 | fetch16(c)); }
static Ea am_absi(Cpu *c, uint16_t i, int w)
{
    uint32_t base = (uint32_t)c->db << 16 | fetch16(c);
    uint32_t a = base + i;
    if (w || !X8 || ((base ^ a) & 0xFF00)) io(c);
    return lin(a);
}
static Ea am_long(Cpu *c) { return lin(fetch24(c)); }
static Ea am_longx(Cpu *c) { return lin(fetch24(c) + c->x); }

static uint8_t ld8(Cpu *c, Ea e) { return rd(c, e.a); }
static uint32_t next(Ea e) { return e.w ? (e.a & 0xFF0000) | ((e.a + 1) & 0xFFFF) : (e.a + 1) & 0xFFFFFF; }
static uint16_t ld16(Cpu *c, Ea e) { uint16_t lo = rd(c, e.a); return lo | rd(c, next(e)) << 8; }
static void st8(Cpu *c, Ea e, uint8_t v) { wr(c, e.a, v); }
static void st16(Cpu *c, Ea e, uint16_t v) { wr(c, e.a, v); wr(c, next(e), v >> 8); }

/* Operand fetch for the M- and X-sized ops; mode < 0 means immediate. */
static uint16_t ldm(Cpu *c, Ea e) { return M8 ? ld8(c, e) : ld16(c, e); }
static uint16_t ldx(Cpu *c, Ea e) { return X8 ? ld8(c, e) : ld16(c, e); }
static uint16_t immm(Cpu *c) { return M8 ? fetch(c) : fetch16(c); }
static uint16_t immx(Cpu *c) { return X8 ? fetch(c) : fetch16(c); }

/* --- ALU --- */
static void op_ora(Cpu *c, uint16_t v)
{
    if (M8) { c->a = (c->a & 0xFF00) | (uint8_t)(c->a | v); nz8(c, c->a); }
    else { c->a |= v; nz16(c, c->a); }
}
static void op_and(Cpu *c, uint16_t v)
{
    if (M8) { c->a = (c->a & 0xFF00) | (uint8_t)(c->a & v); nz8(c, c->a); }
    else { c->a &= v; nz16(c, c->a); }
}
static void op_eor(Cpu *c, uint16_t v)
{
    if (M8) { c->a = (c->a & 0xFF00) | (uint8_t)(c->a ^ v); nz8(c, c->a); }
    else { c->a ^= v; nz16(c, c->a); }
}
static void op_lda(Cpu *c, uint16_t v)
{
    if (M8) { c->a = (c->a & 0xFF00) | (uint8_t)v; nz8(c, v); }
    else { c->a = v; nz16(c, v); }
}
static void op_ldx(Cpu *c, uint16_t v) { c->x = v; if (X8) nz8(c, v); else nz16(c, v); }
static void op_ldy(Cpu *c, uint16_t v) { c->y = v; if (X8) nz8(c, v); else nz16(c, v); }

static void cmp(Cpu *c, uint16_t r, uint16_t v, int b8)
{
    int t = b8 ? (r & 0xFF) - (v & 0xFF) : r - v;
    c->p = (c->p & ~FC) | (t >= 0 ? FC : 0);
    if (b8) nz8(c, t); else nz16(c, t);
}
static void op_cmp(Cpu *c, uint16_t v) { cmp(c, c->a, v, M8); }
static void op_cpx(Cpu *c, uint16_t v) { cmp(c, c->x, v, X8); }
static void op_cpy(Cpu *c, uint16_t v) { cmp(c, c->y, v, X8); }

static void op_bit(Cpu *c, uint16_t v)
{
    if (M8) {
        c->p = (c->p & ~(FN | FV | FZ)) | (v & (FN | FV)) | ((c->a & v & 0xFF) ? 0 : FZ);
    } else {
        c->p = (c->p & ~(FN | FV | FZ)) | (v >> 8 & (FN | FV)) | ((c->a & v) ? 0 : FZ);
    }
}
static void op_bit_imm(Cpu *c, uint16_t v)
{
    uint16_t r = c->a & v;
    if (M8) r &= 0xFF;
    c->p = (c->p & ~FZ) | (r ? 0 : FZ);
}

static void op_adc(Cpu *c, uint16_t v)
{
    int carry = c->p & FC, r;
    if (M8) {
        int a = c->a & 0xFF;
        v &= 0xFF;
        if (!(c->p & FD)) r = a + v + carry;
        else {
            r = (a & 0x0F) + (v & 0x0F) + carry;
            if (r > 0x09) r += 0x06;
            carry = r > 0x0F;
            r = (a & 0xF0) + (v & 0xF0) + (carry << 4) + (r & 0x0F);
        }
        c->p = (c->p & ~FV) | ((~(a ^ v) & (a ^ r) & 0x80) ? FV : 0);
        if ((c->p & FD) && r > 0x9F) r += 0x60;
        c->p = (c->p & ~FC) | (r > 0xFF ? FC : 0);
        c->a = (c->a & 0xFF00) | (uint8_t)r;
        nz8(c, r);
    } else {
        int a = c->a;
        if (!(c->p & FD)) r = a + v + carry;
        else {
            r = (a & 0x000F) + (v & 0x000F) + carry;
            if (r > 0x0009) r += 0x0006;
            carry = r > 0x000F;
            r = (a & 0x00F0) + (v & 0x00F0) + (carry << 4) + (r & 0x000F);
            if (r > 0x009F) r += 0x0060;
            carry = r > 0x00FF;
            r = (a & 0x0F00) + (v & 0x0F00) + (carry << 8) + (r & 0x00FF);
            if (r > 0x09FF) r += 0x0600;
            carry = r > 0x0FFF;
            r = (a & 0xF000) + (v & 0xF000) + (carry << 12) + (r & 0x0FFF);
        }
        c->p = (c->p & ~FV) | ((~(a ^ v) & (a ^ r) & 0x8000) ? FV : 0);
        if ((c->p & FD) && r > 0x9FFF) r += 0x6000;
        c->p = (c->p & ~FC) | (r > 0xFFFF ? FC : 0);
        c->a = r;
        nz16(c, r);
    }
}

static void op_sbc(Cpu *c, uint16_t v)
{
    int carry = c->p & FC, r;
    if (M8) {
        int a = c->a & 0xFF;
        v = ~v & 0xFF;
        if (!(c->p & FD)) r = a + v + carry;
        else {
            r = (a & 0x0F) + (v & 0x0F) + carry;
            if (r <= 0x0F) r -= 0x06;
            carry = r > 0x0F;
            r = (a & 0xF0) + (v & 0xF0) + (carry << 4) + (r & 0x0F);
        }
        c->p = (c->p & ~FV) | ((~(a ^ v) & (a ^ r) & 0x80) ? FV : 0);
        if ((c->p & FD) && r <= 0xFF) r -= 0x60;
        c->p = (c->p & ~FC) | (r > 0xFF ? FC : 0);
        c->a = (c->a & 0xFF00) | (uint8_t)r;
        nz8(c, r);
    } else {
        int a = c->a;
        v = ~v;
        if (!(c->p & FD)) r = a + v + carry;
        else {
            r = (a & 0x000F) + (v & 0x000F) + carry;
            if (r <= 0x000F) r -= 0x0006;
            carry = r > 0x000F;
            r = (a & 0x00F0) + (v & 0x00F0) + (carry << 4) + (r & 0x000F);
            if (r <= 0x00FF) r -= 0x0060;
            carry = r > 0x00FF;
            r = (a & 0x0F00) + (v & 0x0F00) + (carry << 8) + (r & 0x00FF);
            if (r <= 0x0FFF) r -= 0x0600;
            carry = r > 0x0FFF;
            r = (a & 0xF000) + (v & 0xF000) + (carry << 12) + (r & 0x0FFF);
        }
        c->p = (c->p & ~FV) | ((~(a ^ v) & (a ^ r) & 0x8000) ? FV : 0);
        if ((c->p & FD) && r <= 0xFFFF) r -= 0x6000;
        c->p = (c->p & ~FC) | (r > 0xFFFF ? FC : 0);
        c->a = r;
        nz16(c, r);
    }
}

/* Read-modify-write ops on a value of width b8 (1 = 8-bit). */
typedef uint16_t (*Rmw)(Cpu *c, uint16_t v, int b8);
static uint16_t rmw_asl(Cpu *c, uint16_t v, int b8)
{
    uint16_t top = b8 ? 0x80 : 0x8000;
    c->p = (c->p & ~FC) | ((v & top) ? FC : 0);
    v <<= 1;
    if (b8) nz8(c, v); else nz16(c, v);
    return v;
}
static uint16_t rmw_lsr(Cpu *c, uint16_t v, int b8)
{
    if (b8) v &= 0xFF;
    c->p = (c->p & ~FC) | (v & 1);
    v >>= 1;
    if (b8) nz8(c, v); else nz16(c, v);
    return v;
}
static uint16_t rmw_rol(Cpu *c, uint16_t v, int b8)
{
    uint16_t top = b8 ? 0x80 : 0x8000;
    uint16_t in = c->p & FC;
    c->p = (c->p & ~FC) | ((v & top) ? FC : 0);
    v = v << 1 | in;
    if (b8) nz8(c, v); else nz16(c, v);
    return v;
}
static uint16_t rmw_ror(Cpu *c, uint16_t v, int b8)
{
    uint16_t in = (c->p & FC) ? (b8 ? 0x80 : 0x8000) : 0;
    if (b8) v &= 0xFF;
    c->p = (c->p & ~FC) | (v & 1);
    v = v >> 1 | in;
    if (b8) nz8(c, v); else nz16(c, v);
    return v;
}
static uint16_t rmw_inc(Cpu *c, uint16_t v, int b8) { v++; if (b8) nz8(c, v); else nz16(c, v); return v; }
static uint16_t rmw_dec(Cpu *c, uint16_t v, int b8) { v--; if (b8) nz8(c, v); else nz16(c, v); return v; }
static uint16_t rmw_tsb(Cpu *c, uint16_t v, int b8)
{
    uint16_t m = b8 ? 0xFF : 0xFFFF;
    c->p = (c->p & ~FZ) | ((c->a & v & m) ? 0 : FZ);
    return v | c->a;
}
static uint16_t rmw_trb(Cpu *c, uint16_t v, int b8)
{
    uint16_t m = b8 ? 0xFF : 0xFFFF;
    c->p = (c->p & ~FZ) | ((c->a & v & m) ? 0 : FZ);
    return v & ~c->a;
}

static void rmw(Cpu *c, Ea e, Rmw f)
{
    if (M8) {
        uint8_t v = ld8(c, e);
        io(c);
        st8(c, e, f(c, v, 1));
    } else {
        uint16_t v = ld16(c, e);
        io(c);
        v = f(c, v, 0);
        wr(c, next(e), v >> 8);   /* the 65816 writes the high byte first */
        wr(c, e.a, v);
    }
}
static void rmw_a(Cpu *c, Rmw f)
{
    io(c);
    if (M8) c->a = (c->a & 0xFF00) | (uint8_t)f(c, c->a, 1);
    else c->a = f(c, c->a, 0);
}

static void stm(Cpu *c, Ea e, uint16_t v) { if (M8) st8(c, e, v); else st16(c, e, v); }
static void stx(Cpu *c, Ea e, uint16_t v) { if (X8) st8(c, e, v); else st16(c, e, v); }

static void branch(Cpu *c, int cond)
{
    int8_t o = fetch(c);
    if (!cond) return;
    uint16_t t = c->pc + o;
    io(c);
    if (c->e && ((t ^ c->pc) & 0xFF00)) io(c);
    c->pc = t;
}

static void interrupt(Cpu *c, uint16_t native, uint16_t emu, int brk)
{
    if (!c->e) push8(c, c->pb);
    push16(c, c->pc);
    push8(c, c->e ? (brk ? c->p | 0x10 : c->p & ~0x10) : c->p);
    c->p = (c->p | FI) & ~FD;
    c->pb = 0;
    uint16_t v = c->e ? emu : native;
    c->pc = rd(c, v) | rd(c, v + 1) << 8;
}

void cpu_reset(Cpu *c)
{
    c->e = 1;
    c->p = FM | FX | FI;
    c->d = 0;
    c->db = 0;
    c->pb = 0;
    c->sp = 0x100 | (c->sp & 0xFF);
    c->x &= 0xFF;
    c->y &= 0xFF;
    c->wai = c->stp = 0;
    c->nmi = 0;
    c->pc = rd(c, 0xFFFC) | rd(c, 0xFFFD) << 8;
}

static void transfer_x(Cpu *c, uint16_t *dst, uint16_t v)
{
    io(c);
    if (X8) { *dst = v & 0xFF; nz8(c, v); }
    else { *dst = v; nz16(c, v); }
}
static void transfer_a(Cpu *c, uint16_t v)
{
    io(c);
    if (M8) { c->a = (c->a & 0xFF00) | (v & 0xFF); nz8(c, v); }
    else { c->a = v; nz16(c, v); }
}

static void block_move(Cpu *c, int dir)
{
    uint8_t dst = fetch(c), src = fetch(c);
    c->db = dst;
    wr(c, (uint32_t)dst << 16 | c->y, rd(c, (uint32_t)src << 16 | c->x));
    io(c); io(c);
    c->x += dir; c->y += dir;
    if (X8) { c->x &= 0xFF; c->y &= 0xFF; }
    if (c->a-- != 0) c->pc -= 3;
}

void cpu_step(Cpu *c)
{
    if (c->stp) { io(c); return; }
    if (c->wai) {
        if (!c->nmi && !c->irq) { io(c); return; }
        c->wai = 0;
        io(c);
    }
    if (c->nmi) {
        c->nmi = 0;
        io(c); io(c);
        interrupt(c, 0xFFEA, 0xFFFA, 0);
        return;
    }
    if (c->irq && !(c->p & FI)) {
        io(c); io(c);
        interrupt(c, 0xFFEE, 0xFFFE, 0);
        return;
    }

    uint8_t op = fetch(c);
    Ea e;
    switch (op) {
    /* ALU group: ora and eor adc sta lda cmp sbc */
#define ALU(base, fn) \
    case base + 0x01: fn(c, ldm(c, am_dpindx(c))); break; \
    case base + 0x03: fn(c, ldm(c, am_sr(c))); break; \
    case base + 0x05: fn(c, ldm(c, am_dp(c))); break; \
    case base + 0x07: fn(c, ldm(c, am_dpindl(c))); break; \
    case base + 0x09: fn(c, immm(c)); break; \
    case base + 0x0D: fn(c, ldm(c, am_abs(c))); break; \
    case base + 0x0F: fn(c, ldm(c, am_long(c))); break; \
    case base + 0x11: fn(c, ldm(c, am_dpindy(c, 0))); break; \
    case base + 0x12: fn(c, ldm(c, am_dpind(c))); break; \
    case base + 0x13: fn(c, ldm(c, am_srindy(c))); break; \
    case base + 0x15: fn(c, ldm(c, am_dpi(c, c->x))); break; \
    case base + 0x17: fn(c, ldm(c, am_dpindly(c))); break; \
    case base + 0x19: fn(c, ldm(c, am_absi(c, c->y, 0))); break; \
    case base + 0x1D: fn(c, ldm(c, am_absi(c, c->x, 0))); break; \
    case base + 0x1F: fn(c, ldm(c, am_longx(c))); break;
    ALU(0x00, op_ora)
    ALU(0x20, op_and)
    ALU(0x40, op_eor)
    ALU(0x60, op_adc)
    ALU(0xA0, op_lda)
    ALU(0xC0, op_cmp)
    ALU(0xE0, op_sbc)
#undef ALU
    case 0x81: stm(c, am_dpindx(c), c->a); break;
    case 0x83: stm(c, am_sr(c), c->a); break;
    case 0x85: stm(c, am_dp(c), c->a); break;
    case 0x87: stm(c, am_dpindl(c), c->a); break;
    case 0x89: op_bit_imm(c, immm(c)); break;
    case 0x8D: stm(c, am_abs(c), c->a); break;
    case 0x8F: stm(c, am_long(c), c->a); break;
    case 0x91: stm(c, am_dpindy(c, 1), c->a); break;
    case 0x92: stm(c, am_dpind(c), c->a); break;
    case 0x93: stm(c, am_srindy(c), c->a); break;
    case 0x95: stm(c, am_dpi(c, c->x), c->a); break;
    case 0x97: stm(c, am_dpindly(c), c->a); break;
    case 0x99: stm(c, am_absi(c, c->y, 1), c->a); break;
    case 0x9D: stm(c, am_absi(c, c->x, 1), c->a); break;
    case 0x9F: stm(c, am_longx(c), c->a); break;

    /* stores of X, Y and zero */
    case 0x86: stx(c, am_dp(c), c->x); break;
    case 0x8E: stx(c, am_abs(c), c->x); break;
    case 0x96: stx(c, am_dpi(c, c->y), c->x); break;
    case 0x84: stx(c, am_dp(c), c->y); break;
    case 0x8C: stx(c, am_abs(c), c->y); break;
    case 0x94: stx(c, am_dpi(c, c->x), c->y); break;
    case 0x64: stm(c, am_dp(c), 0); break;
    case 0x74: stm(c, am_dpi(c, c->x), 0); break;
    case 0x9C: stm(c, am_abs(c), 0); break;
    case 0x9E: stm(c, am_absi(c, c->x, 1), 0); break;

    /* loads / compares of X and Y */
    case 0xA2: op_ldx(c, immx(c)); break;
    case 0xA6: op_ldx(c, ldx(c, am_dp(c))); break;
    case 0xAE: op_ldx(c, ldx(c, am_abs(c))); break;
    case 0xB6: op_ldx(c, ldx(c, am_dpi(c, c->y))); break;
    case 0xBE: op_ldx(c, ldx(c, am_absi(c, c->y, 0))); break;
    case 0xA0: op_ldy(c, immx(c)); break;
    case 0xA4: op_ldy(c, ldx(c, am_dp(c))); break;
    case 0xAC: op_ldy(c, ldx(c, am_abs(c))); break;
    case 0xB4: op_ldy(c, ldx(c, am_dpi(c, c->x))); break;
    case 0xBC: op_ldy(c, ldx(c, am_absi(c, c->x, 0))); break;
    case 0xE0: op_cpx(c, immx(c)); break;
    case 0xE4: op_cpx(c, ldx(c, am_dp(c))); break;
    case 0xEC: op_cpx(c, ldx(c, am_abs(c))); break;
    case 0xC0: op_cpy(c, immx(c)); break;
    case 0xC4: op_cpy(c, ldx(c, am_dp(c))); break;
    case 0xCC: op_cpy(c, ldx(c, am_abs(c))); break;

    /* bit */
    case 0x24: op_bit(c, ldm(c, am_dp(c))); break;
    case 0x2C: op_bit(c, ldm(c, am_abs(c))); break;
    case 0x34: op_bit(c, ldm(c, am_dpi(c, c->x))); break;
    case 0x3C: op_bit(c, ldm(c, am_absi(c, c->x, 0))); break;

    /* read-modify-write */
#define RMW(dp, acc, ab, dpx, abx, fn) \
    case dp: rmw(c, am_dp(c), fn); break; \
    case acc: rmw_a(c, fn); break; \
    case ab: rmw(c, am_abs(c), fn); break; \
    case dpx: rmw(c, am_dpi(c, c->x), fn); break; \
    case abx: rmw(c, am_absi(c, c->x, 1), fn); break;
    RMW(0x06, 0x0A, 0x0E, 0x16, 0x1E, rmw_asl)
    RMW(0x26, 0x2A, 0x2E, 0x36, 0x3E, rmw_rol)
    RMW(0x46, 0x4A, 0x4E, 0x56, 0x5E, rmw_lsr)
    RMW(0x66, 0x6A, 0x6E, 0x76, 0x7E, rmw_ror)
    RMW(0xE6, 0x1A, 0xEE, 0xF6, 0xFE, rmw_inc)
    RMW(0xC6, 0x3A, 0xCE, 0xD6, 0xDE, rmw_dec)
#undef RMW
    case 0x04: rmw(c, am_dp(c), rmw_tsb); break;
    case 0x0C: rmw(c, am_abs(c), rmw_tsb); break;
    case 0x14: rmw(c, am_dp(c), rmw_trb); break;
    case 0x1C: rmw(c, am_abs(c), rmw_trb); break;

    /* index increments */
    case 0xE8: io(c); c->x++; if (X8) { c->x &= 0xFF; nz8(c, c->x); } else nz16(c, c->x); break;
    case 0xC8: io(c); c->y++; if (X8) { c->y &= 0xFF; nz8(c, c->y); } else nz16(c, c->y); break;
    case 0xCA: io(c); c->x--; if (X8) { c->x &= 0xFF; nz8(c, c->x); } else nz16(c, c->x); break;
    case 0x88: io(c); c->y--; if (X8) { c->y &= 0xFF; nz8(c, c->y); } else nz16(c, c->y); break;

    /* branches and jumps */
    case 0x10: branch(c, !(c->p & FN)); break;
    case 0x30: branch(c, c->p & FN); break;
    case 0x50: branch(c, !(c->p & FV)); break;
    case 0x70: branch(c, c->p & FV); break;
    case 0x90: branch(c, !(c->p & FC)); break;
    case 0xB0: branch(c, c->p & FC); break;
    case 0xD0: branch(c, !(c->p & FZ)); break;
    case 0xF0: branch(c, c->p & FZ); break;
    case 0x80: branch(c, 1); break;
    case 0x82: { uint16_t o = fetch16(c); io(c); c->pc += o; break; }
    case 0x4C: c->pc = fetch16(c); break;
    case 0x5C: { uint32_t t = fetch24(c); c->pc = t; c->pb = t >> 16; break; }
    case 0x6C: c->pc = ptr16(c, fetch16(c)); break;
    case 0x7C: {
        uint16_t p = fetch16(c) + c->x;
        io(c);
        uint32_t b = (uint32_t)c->pb << 16;
        c->pc = rd(c, b | p) | rd(c, b | (uint16_t)(p + 1)) << 8;
        break;
    }
    case 0xDC: { uint32_t t = ptr24(c, fetch16(c)); c->pc = t; c->pb = t >> 16; break; }
    case 0x20: { uint16_t t = fetch16(c); io(c); push16(c, c->pc - 1); c->pc = t; break; }
    case 0x22: {
        uint16_t t = fetch16(c);
        push8(c, c->pb);
        io(c);
        uint8_t b = fetch(c);
        push16(c, c->pc - 1);
        c->pc = t; c->pb = b;
        break;
    }
    case 0xFC: {
        uint8_t lo = fetch(c);
        push16(c, c->pc);   /* = address of the high operand byte */
        uint16_t p = (lo | fetch(c) << 8) + c->x;
        io(c);
        uint32_t b = (uint32_t)c->pb << 16;
        c->pc = rd(c, b | p) | rd(c, b | (uint16_t)(p + 1)) << 8;
        break;
    }
    case 0x60: io(c); io(c); c->pc = pull16(c) + 1; io(c); break;
    case 0x6B: io(c); io(c); c->pc = pull16(c) + 1; c->pb = pull8(c); break;
    case 0x40:
        io(c); io(c);
        c->p = pull8(c);
        if (c->e) c->p |= FM | FX;
        fix_p(c);
        c->pc = pull16(c);
        if (!c->e) c->pb = pull8(c);
        break;
    case 0x00: fetch(c); interrupt(c, 0xFFE6, 0xFFFE, 1); break;
    case 0x02: fetch(c); interrupt(c, 0xFFE4, 0xFFF4, 1); break;

    /* flags */
    case 0x18: io(c); c->p &= ~FC; break;
    case 0x38: io(c); c->p |= FC; break;
    case 0x58: io(c); c->p &= ~FI; break;
    case 0x78: io(c); c->p |= FI; break;
    case 0xB8: io(c); c->p &= ~FV; break;
    case 0xD8: io(c); c->p &= ~FD; break;
    case 0xF8: io(c); c->p |= FD; break;
    case 0xC2: c->p &= ~fetch(c); io(c); fix_p(c); break;
    case 0xE2: c->p |= fetch(c); io(c); fix_p(c); break;
    case 0xFB: {
        io(c);
        uint8_t carry = c->p & FC;
        c->p = (c->p & ~FC) | c->e;
        c->e = carry;
        if (c->e) { c->p |= FM | FX; c->sp = 0x100 | (c->sp & 0xFF); }
        fix_p(c);
        break;
    }

    /* transfers */
    case 0xAA: transfer_x(c, &c->x, c->a); break;
    case 0xA8: transfer_x(c, &c->y, c->a); break;
    case 0xBA: transfer_x(c, &c->x, c->sp); break;
    case 0x9B: transfer_x(c, &c->y, c->x); break;
    case 0xBB: transfer_x(c, &c->x, c->y); break;
    case 0x8A: transfer_a(c, c->x); break;
    case 0x98: transfer_a(c, c->y); break;
    case 0x9A: io(c); c->sp = c->e ? 0x100 | (c->x & 0xFF) : c->x; break;
    case 0x1B: io(c); c->sp = c->e ? 0x100 | (c->a & 0xFF) : c->a; break;
    case 0x3B: io(c); c->a = c->sp; nz16(c, c->a); break;
    case 0x5B: io(c); c->d = c->a; nz16(c, c->d); break;
    case 0x7B: io(c); c->a = c->d; nz16(c, c->a); break;
    case 0xEB: io(c); io(c); c->a = c->a >> 8 | c->a << 8; nz8(c, c->a); break;

    /* stack */
    case 0x48: io(c); if (M8) push8(c, c->a); else push16(c, c->a); break;
    case 0xDA: io(c); if (X8) push8(c, c->x); else push16(c, c->x); break;
    case 0x5A: io(c); if (X8) push8(c, c->y); else push16(c, c->y); break;
    case 0x08: io(c); push8(c, c->p); break;
    case 0x8B: io(c); push8(c, c->db); break;
    case 0x4B: io(c); push8(c, c->pb); break;
    case 0x0B: io(c); push16(c, c->d); break;
    case 0x68: io(c); io(c);
        if (M8) { c->a = (c->a & 0xFF00) | pull8(c); nz8(c, c->a); }
        else { c->a = pull16(c); nz16(c, c->a); }
        break;
    case 0xFA: io(c); io(c);
        if (X8) { c->x = pull8(c); nz8(c, c->x); } else { c->x = pull16(c); nz16(c, c->x); }
        break;
    case 0x7A: io(c); io(c);
        if (X8) { c->y = pull8(c); nz8(c, c->y); } else { c->y = pull16(c); nz16(c, c->y); }
        break;
    case 0x28: io(c); io(c); c->p = pull8(c); fix_p(c); break;
    case 0xAB: io(c); io(c); c->db = pull8(c); nz8(c, c->db); break;
    case 0x2B: io(c); io(c); c->d = pull16(c); nz16(c, c->d); break;
    case 0xF4: push16(c, fetch16(c)); break;
    case 0xD4: e = am_dp(c); push16(c, ptr16(c, e.a)); break;
    case 0x62: { uint16_t o = fetch16(c); io(c); push16(c, c->pc + o); break; }

    /* misc */
    case 0x54: block_move(c, 1); break;    /* mvn */
    case 0x44: block_move(c, -1); break;   /* mvp */
    case 0xEA: io(c); break;
    case 0x42: fetch(c); break;            /* wdm */
    case 0xCB: io(c); io(c); c->wai = 1; break;
    case 0xDB: io(c); io(c); c->stp = 1; break;
    default:
        fprintf(stderr, "cpu: unhandled opcode %02X\n", op);
        break;
    }
}

/* --- disassembly table --- */
const char cpu_mnemonic[256][4] = {
    "brk","ora","cop","ora","tsb","ora","asl","ora","php","ora","asl","phd","tsb","ora","asl","ora",
    "bpl","ora","ora","ora","trb","ora","asl","ora","clc","ora","inc","tcs","trb","ora","asl","ora",
    "jsr","and","jsl","and","bit","and","rol","and","plp","and","rol","pld","bit","and","rol","and",
    "bmi","and","and","and","bit","and","rol","and","sec","and","dec","tsc","bit","and","rol","and",
    "rti","eor","wdm","eor","mvp","eor","lsr","eor","pha","eor","lsr","phk","jmp","eor","lsr","eor",
    "bvc","eor","eor","eor","mvn","eor","lsr","eor","cli","eor","phy","tcd","jml","eor","lsr","eor",
    "rts","adc","per","adc","stz","adc","ror","adc","pla","adc","ror","rtl","jmp","adc","ror","adc",
    "bvs","adc","adc","adc","stz","adc","ror","adc","sei","adc","ply","tdc","jmp","adc","ror","adc",
    "bra","sta","brl","sta","sty","sta","stx","sta","dey","bit","txa","phb","sty","sta","stx","sta",
    "bcc","sta","sta","sta","sty","sta","stx","sta","tya","sta","txs","txy","stz","sta","stz","sta",
    "ldy","lda","ldx","lda","ldy","lda","ldx","lda","tay","lda","tax","plb","ldy","lda","ldx","lda",
    "bcs","lda","lda","lda","ldy","lda","ldx","lda","clv","lda","tsx","tyx","ldy","lda","ldx","lda",
    "cpy","cmp","rep","cmp","cpy","cmp","dec","cmp","iny","cmp","dex","wai","cpy","cmp","dec","cmp",
    "bne","cmp","cmp","cmp","pei","cmp","dec","cmp","cld","cmp","phx","stp","jml","cmp","dec","cmp",
    "cpx","sbc","sep","sbc","cpx","sbc","inc","sbc","inx","sbc","nop","xba","cpx","sbc","inc","sbc",
    "beq","sbc","sbc","sbc","pea","sbc","inc","sbc","sed","sbc","plx","xce","jsr","sbc","inc","sbc",
};

#define I AM_IMP
#define A AM_ACC
const uint8_t cpu_mode[256] = {
 AM_IMM8,AM_DPINDX,AM_IMM8,AM_SR,AM_DP,AM_DP,AM_DP,AM_DPINDL,I,AM_IMM_M,A,I,AM_ABS,AM_ABS,AM_ABS,AM_LONG,
 AM_REL,AM_DPINDY,AM_DPIND,AM_SRINDY,AM_DP,AM_DPX,AM_DPX,AM_DPINDLY,I,AM_ABSY,A,I,AM_ABS,AM_ABSX,AM_ABSX,AM_LONGX,
 AM_ABS,AM_DPINDX,AM_LONG,AM_SR,AM_DP,AM_DP,AM_DP,AM_DPINDL,I,AM_IMM_M,A,I,AM_ABS,AM_ABS,AM_ABS,AM_LONG,
 AM_REL,AM_DPINDY,AM_DPIND,AM_SRINDY,AM_DPX,AM_DPX,AM_DPX,AM_DPINDLY,I,AM_ABSY,A,I,AM_ABSX,AM_ABSX,AM_ABSX,AM_LONGX,
 I,AM_DPINDX,AM_IMM8,AM_SR,AM_MOVE,AM_DP,AM_DP,AM_DPINDL,I,AM_IMM_M,A,I,AM_ABS,AM_ABS,AM_ABS,AM_LONG,
 AM_REL,AM_DPINDY,AM_DPIND,AM_SRINDY,AM_MOVE,AM_DPX,AM_DPX,AM_DPINDLY,I,AM_ABSY,I,I,AM_LONG,AM_ABSX,AM_ABSX,AM_LONGX,
 I,AM_DPINDX,AM_RELL,AM_SR,AM_DP,AM_DP,AM_DP,AM_DPINDL,I,AM_IMM_M,A,I,AM_ABSIND,AM_ABS,AM_ABS,AM_LONG,
 AM_REL,AM_DPINDY,AM_DPIND,AM_SRINDY,AM_DPX,AM_DPX,AM_DPX,AM_DPINDLY,I,AM_ABSY,I,I,AM_ABSINDX,AM_ABSX,AM_ABSX,AM_LONGX,
 AM_REL,AM_DPINDX,AM_RELL,AM_SR,AM_DP,AM_DP,AM_DP,AM_DPINDL,I,AM_IMM_M,I,I,AM_ABS,AM_ABS,AM_ABS,AM_LONG,
 AM_REL,AM_DPINDY,AM_DPIND,AM_SRINDY,AM_DPX,AM_DPX,AM_DPY,AM_DPINDLY,I,AM_ABSY,I,I,AM_ABS,AM_ABSX,AM_ABSX,AM_LONGX,
 AM_IMM_X,AM_DPINDX,AM_IMM_X,AM_SR,AM_DP,AM_DP,AM_DP,AM_DPINDL,I,AM_IMM_M,I,I,AM_ABS,AM_ABS,AM_ABS,AM_LONG,
 AM_REL,AM_DPINDY,AM_DPIND,AM_SRINDY,AM_DPX,AM_DPX,AM_DPY,AM_DPINDLY,I,AM_ABSY,I,I,AM_ABSX,AM_ABSX,AM_ABSY,AM_LONGX,
 AM_IMM_X,AM_DPINDX,AM_IMM8,AM_SR,AM_DP,AM_DP,AM_DP,AM_DPINDL,I,AM_IMM_M,I,I,AM_ABS,AM_ABS,AM_ABS,AM_LONG,
 AM_REL,AM_DPINDY,AM_DPIND,AM_SRINDY,AM_DP,AM_DPX,AM_DPX,AM_DPINDLY,I,AM_ABSY,I,I,AM_ABSINDL,AM_ABSX,AM_ABSX,AM_LONGX,
 AM_IMM_X,AM_DPINDX,AM_IMM8,AM_SR,AM_DP,AM_DP,AM_DP,AM_DPINDL,I,AM_IMM_M,I,I,AM_ABS,AM_ABS,AM_ABS,AM_LONG,
 AM_REL,AM_DPINDY,AM_DPIND,AM_SRINDY,AM_ABS,AM_DPX,AM_DPX,AM_DPINDLY,I,AM_ABSY,I,I,AM_ABSINDX,AM_ABSX,AM_ABSX,AM_LONGX,
};
#undef I
#undef A

int cpu_disasm(char *buf, uint32_t addr, int m8, int x8,
               uint8_t (*rdf)(void *ctx, uint32_t addr), void *ctx)
{
    uint8_t op = rdf(ctx, addr);
    int mode = cpu_mode[op], n;
    switch (mode) {
    case AM_IMP: case AM_ACC: n = 0; break;
    case AM_IMM_M: n = m8 ? 1 : 2; break;
    case AM_IMM_X: n = x8 ? 1 : 2; break;
    case AM_ABS: case AM_ABSX: case AM_ABSY: case AM_ABSIND: case AM_ABSINDX:
    case AM_ABSINDL: case AM_RELL: case AM_MOVE: n = 2; break;
    case AM_LONG: case AM_LONGX: n = 3; break;
    default: n = 1; break;
    }
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)rdf(ctx, (addr & 0xFF0000) | ((addr + 1 + i) & 0xFFFF)) << (8 * i);
    const char *mn = cpu_mnemonic[op];
    uint16_t pc = addr + 1 + n;
    switch (mode) {
    case AM_IMP: sprintf(buf, "%s", mn); break;
    case AM_ACC: sprintf(buf, "%s A", mn); break;
    case AM_IMM_M: case AM_IMM_X: case AM_IMM8:
        sprintf(buf, n == 1 ? "%s #$%02X" : "%s #$%04X", mn, v); break;
    case AM_DP: sprintf(buf, "%s $%02X", mn, v); break;
    case AM_DPX: sprintf(buf, "%s $%02X,x", mn, v); break;
    case AM_DPY: sprintf(buf, "%s $%02X,y", mn, v); break;
    case AM_DPIND: sprintf(buf, "%s ($%02X)", mn, v); break;
    case AM_DPINDX: sprintf(buf, "%s ($%02X,x)", mn, v); break;
    case AM_DPINDY: sprintf(buf, "%s ($%02X),y", mn, v); break;
    case AM_DPINDL: sprintf(buf, "%s [$%02X]", mn, v); break;
    case AM_DPINDLY: sprintf(buf, "%s [$%02X],y", mn, v); break;
    case AM_SR: sprintf(buf, "%s $%02X,s", mn, v); break;
    case AM_SRINDY: sprintf(buf, "%s ($%02X,s),y", mn, v); break;
    case AM_ABS: sprintf(buf, "%s $%04X", mn, v); break;
    case AM_ABSX: sprintf(buf, "%s $%04X,x", mn, v); break;
    case AM_ABSY: sprintf(buf, "%s $%04X,y", mn, v); break;
    case AM_LONG: sprintf(buf, "%s $%06X", mn, v); break;
    case AM_LONGX: sprintf(buf, "%s $%06X,x", mn, v); break;
    case AM_ABSIND: sprintf(buf, "%s ($%04X)", mn, v); break;
    case AM_ABSINDX: sprintf(buf, "%s ($%04X,x)", mn, v); break;
    case AM_ABSINDL: sprintf(buf, "%s [$%04X]", mn, v); break;
    case AM_REL: sprintf(buf, "%s $%04X", mn, (uint16_t)(pc + (int8_t)v)); break;
    case AM_RELL: sprintf(buf, "%s $%04X", mn, (uint16_t)(pc + (int16_t)v)); break;
    case AM_MOVE: sprintf(buf, "%s $%02X,$%02X", mn, v & 0xFF, v >> 8); break;
    }
    return 1 + n;
}
