/* WDC 65816 interpreter. Instruction-stepped; the bus callbacks see every
 * memory access and idle() is called once per internal operation cycle, so a
 * bus can count time with them (cpu.c itself has no notion of cycles). */
#ifndef CPU_H
#define CPU_H
#include <stdint.h>

enum { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FX = 0x10, FM = 0x20,
       FV = 0x40, FN = 0x80 };

typedef struct Cpu {
    uint16_t a, x, y, sp, d, pc;
    uint8_t pb, db, p, e;   /* p = NVMXDIZC; in emulation mode M = X = 1 */
    uint8_t nmi;            /* edge latched by the bus, cleared when taken */
    uint8_t irq;            /* level, owned by the bus */
    uint8_t wai, stp;
    void *ctx;
    uint8_t (*read)(void *ctx, uint32_t addr);
    void (*write)(void *ctx, uint32_t addr, uint8_t v);
    void (*idle)(void *ctx);
} Cpu;

void cpu_reset(Cpu *c);
/* Runs one instruction, or takes a pending interrupt, or idles one cycle
 * while stopped / waiting. */
void cpu_step(Cpu *c);

/* Disassembly: addressing modes and their operand sizes. */
enum { AM_IMP, AM_ACC, AM_IMM_M, AM_IMM_X, AM_IMM8, AM_DP, AM_DPX, AM_DPY,
       AM_DPIND, AM_DPINDX, AM_DPINDY, AM_DPINDL, AM_DPINDLY, AM_SR,
       AM_SRINDY, AM_ABS, AM_ABSX, AM_ABSY, AM_LONG, AM_LONGX, AM_ABSIND,
       AM_ABSINDX, AM_ABSINDL, AM_REL, AM_RELL, AM_MOVE };
extern const char cpu_mnemonic[256][4];
extern const uint8_t cpu_mode[256];
/* Formats the instruction at addr (m8/x8 = current widths) into buf;
 * returns its length in bytes. rd reads one byte. */
int cpu_disasm(char *buf, uint32_t addr, int m8, int x8,
               uint8_t (*rd)(void *ctx, uint32_t addr), void *ctx);

#endif
