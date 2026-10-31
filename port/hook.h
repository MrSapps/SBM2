/* Native C replacements for ROM routines ("hooks").
 *
 * When the interpreter is about to execute an instruction at a hooked
 * address, the C function runs instead. It sees the machine through the
 * globals and helpers below, and must leave registers, flags and the stack as
 * the original would (at least where callers rely on them), normally by
 * ending with Ret_RTL()/Ret_RTS(), or JumpLong() for a tail jump.
 *
 * Hooks assume native mode, D = 0 and the M/X widths given in their table
 * entry; otherwise the original code runs (counted as a fallback). */
#ifndef HOOK_H
#define HOOK_H
#include <stdint.h>
#include "snes.h"

typedef struct Hook {
    uint32_t addr;
    const char *name;
    void (*fn)(void);
    uint8_t p_clear, p_set;   /* P bits that must be 0 / 1 on entry */
    uint8_t ignore;           /* outputs the shadow check skips (IGN_*) */
} Hook;

enum { IGN_A = 1, IGN_X = 2, IGN_Y = 4, IGN_NZ = 8, IGN_C = 16, IGN_V = 32 };

extern Snes *g_snes;
extern Cpu *g_cpu;
extern uint8_t *g_ram;

/* WRAM by 24-bit address ($7E/$7F, or $00-$3F:0000-1FFF), no side effects. */
static inline uint8_t ram8(uint32_t a)
{
    return g_ram[(a & 0xFF0000) >= 0x7E0000 ? a & 0x1FFFF : a & 0x1FFF];
}
static inline uint32_t ram_ofs(uint32_t a)
{
    return (a & 0xFF0000) >= 0x7E0000 ? a & 0x1FFFF : a & 0x1FFF;
}
static inline uint16_t ram16(uint32_t a)
{
    uint32_t o = ram_ofs(a);
    return g_ram[o] | g_ram[(o + 1) & 0x1FFFF] << 8;
}
static inline void ram8_set(uint32_t a, uint8_t v) { g_ram[ram_ofs(a)] = v; }
static inline void ram16_set(uint32_t a, uint16_t v)
{
    uint32_t o = ram_ofs(a);
    g_ram[o] = v;
    g_ram[(o + 1) & 0x1FFFF] = v >> 8;
}

/* Any 24-bit address through the bus (registers, DMA, ROM, open bus...). */
uint8_t ReadLong8(uint32_t addr);
uint16_t ReadLong16(uint32_t addr);
void WriteLong8(uint32_t addr, uint8_t v);
void WriteLong16(uint32_t addr, uint16_t v);
/* ROM / WRAM without side effects. */
uint8_t PeekLong8(uint32_t addr);
uint16_t PeekLong16(uint32_t addr);
uint32_t PeekLong24(uint32_t addr);

/* Flags. */
void SetNZ8(uint8_t v);
void SetNZ16(uint16_t v);
void SetFlag(uint8_t flag, int on);
static inline int GetFlag(uint8_t flag) { return (g_cpu->p & flag) != 0; }
/* Register widths (as rep/sep would). */
void SetM8(int on);
void SetX8(int on);

/* Control flow. */
void Push8(uint8_t v);
uint8_t Pull8(void);
void Ret_RTL(void);
void Ret_RTS(void);
void JumpLong(uint32_t addr);
/* JSL into ROM code (interpreted, hooks still active) and wait for its RTL. */
void CallLong(uint32_t addr);

/* Table of all hooks (game.c). */
extern const Hook g_hooks[];
extern const int g_nhooks;

/* Installs the dispatcher on s. enable: per-hook flags (NULL = all). */
void hook_install(Snes *s, const uint8_t *enable);
/* Shadow check: each hooked call is also run by the interpreter on a copy of
 * the machine and the results compared (registers, WRAM, PPU registers). */
extern int g_hook_check;
extern int g_hook_check_failed;
/* Statistics. */
extern long *g_hook_calls, *g_hook_fallbacks;
extern int g_last_hook;
int hook_find(const char *name);

#endif
