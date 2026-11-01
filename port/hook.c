/* Hook dispatcher and the API the native routines use (see hook.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hook.h"

Snes *g_snes;
Cpu *g_cpu;
uint8_t *g_ram;

int g_hook_check, g_hook_check_failed;
long *g_hook_calls, *g_hook_fallbacks;
int g_last_hook = -1;

static uint8_t *s_enabled;
static uint8_t s_at[0x10000];   /* 1 if some hook has this low 16-bit address */
static int s_depth;

uint8_t ReadLong8(uint32_t a) { return snes_read(g_snes, a & 0xFFFFFF); }
uint16_t ReadLong16(uint32_t a) { uint16_t lo = ReadLong8(a); return lo | ReadLong8(a + 1) << 8; }
void WriteLong8(uint32_t a, uint8_t v) { snes_write(g_snes, a & 0xFFFFFF, v); }
void WriteLong16(uint32_t a, uint16_t v) { WriteLong8(a, v); WriteLong8(a + 1, v >> 8); }
uint8_t PeekLong8(uint32_t a) { return snes_peek(g_snes, a & 0xFFFFFF); }
uint16_t PeekLong16(uint32_t a) { return PeekLong8(a) | PeekLong8(a + 1) << 8; }
uint32_t PeekLong24(uint32_t a) { return PeekLong16(a) | (uint32_t)PeekLong8(a + 2) << 16; }

void SetFlag(uint8_t flag, int on)
{
    if (on) g_cpu->p |= flag;
    else g_cpu->p &= ~flag;
}
void SetNZ8(uint8_t v) { SetFlag(FN, v & 0x80); SetFlag(FZ, !v); }
void SetNZ16(uint16_t v) { SetFlag(FN, v & 0x8000); SetFlag(FZ, !v); }
void SetM8(int on) { SetFlag(FM, on); }
void SetX8(int on)
{
    SetFlag(FX, on);
    if (on) { g_cpu->x &= 0xFF; g_cpu->y &= 0xFF; }
}

void Push8(uint8_t v) { WriteLong8(g_cpu->sp, v); g_cpu->sp--; }
uint8_t Pull8(void) { g_cpu->sp++; return ReadLong8(g_cpu->sp); }

void Ret_RTL(void)
{
    uint16_t lo = Pull8();
    g_cpu->pc = (lo | Pull8() << 8) + 1;
    g_cpu->pb = Pull8();
}

void Ret_RTS(void)
{
    uint16_t lo = Pull8();
    g_cpu->pc = (lo | Pull8() << 8) + 1;
}

void JumpLong(uint32_t addr)
{
    g_cpu->pb = addr >> 16;
    g_cpu->pc = addr;
}

/* The callee returns to $7F:FFFF (never code), which ends the call. */
void CallLong(uint32_t addr)
{
    Snes *s = g_snes;
    Cpu *c = g_cpu;
    uint8_t *ram = g_ram;
    uint16_t sp0 = c->sp;
    Push8(0x7F);
    Push8(0xFF);
    Push8(0xFE);
    JumpLong(addr);
    /* (s_depth is left alone: inside a shadow-checked call it is already
     * up; under a NO_SHADOW driver the hooks reached here get checked.) */
    while (!(c->pb == 0x7F && c->pc == 0xFFFF && c->sp == sp0)) snes_step(s);
    g_snes = s; g_cpu = c; g_ram = ram;
}

void (*g_driver)(void);
int g_resuming;
static void (*s_resume_fn)(void);
static uint16_t s_resume_sp;
static uint32_t s_resume_pc;

int CallLongR(uint32_t addr, uint32_t ret)
{
    Snes *s = g_snes;
    Cpu *c = g_cpu;
    uint8_t *ram = g_ram;
    uint16_t sp0 = c->sp;
    void (*driver)(void) = g_driver;
    Push8(ret >> 16);
    Push8((ret - 1) >> 8);
    Push8(ret - 1);
    JumpLong(addr);
    while (!(c->pb == (uint8_t)(ret >> 16) && c->pc == (uint16_t)ret && c->sp == sp0)) {
        snes_step(s);
        uint32_t pc = (uint32_t)c->pb << 16 | c->pc;
        if (s->wait_at[pc & 0xFFFF] && snes_wait_point(s, pc) == WAIT_EXIT) {
            s_resume_fn = driver;      /* resumed when the callee returns */
            s_resume_sp = sp0;
            s_resume_pc = ret;
            g_snes = s; g_cpu = c; g_ram = ram;
            return 0;
        }
    }
    g_snes = s; g_cpu = c; g_ram = ram;
    return 1;
}

int hook_find(const char *name)
{
    for (int i = 0; i < g_nhooks; i++) if (!strcmp(g_hooks[i].name, name)) return i;
    return -1;
}

/* --- shadow check --- */
static Snes s_shadow;

static int report(const Hook *h, const char *what, unsigned want, unsigned got)
{
    if (!g_hook_check_failed)
        printf("hook %s (%06X): %s: interpreter %X, native %X\n", h->name, h->addr, what, want, got);
    g_hook_check_failed = 1;
    return 1;
}

static void check_call(Snes *s, int i)
{
    const Hook *h = &g_hooks[i];
    memcpy(&s_shadow, s, sizeof s_shadow);
    s_shadow.cpu.ctx = &s_shadow;
    s_shadow.hook = NULL;
    /* Native code cannot take an NMI halfway; keep both sides NMI-free. */
    s_shadow.fs_hold = 1;
    uint8_t hold = s->fs_hold;
    s->fs_hold = 1;

    s_depth++;
    h->fn();
    s_depth--;
    s->fs_hold = hold;

    Cpu *n = &s->cpu, *r = &s_shadow.cpu;
    long steps = 0;
    do {
        snes_step(&s_shadow);
    } while (!(r->pc == n->pc && r->pb == n->pb && r->sp == n->sp) && ++steps < 10000000);
    if (steps >= 10000000) { report(h, "interpreter never reached native exit pc", r->pc, n->pc); return; }

    uint8_t pmask = 0xFF;
    if (h->ignore & IGN_NZ) pmask &= ~(FN | FZ);
    if (h->ignore & IGN_C) pmask &= ~FC;
    if (h->ignore & IGN_V) pmask &= ~FV;
    if (!(h->ignore & IGN_A) && r->a != n->a) report(h, "A", r->a, n->a);
    if (!(h->ignore & IGN_X) && r->x != n->x) report(h, "X", r->x, n->x);
    if (!(h->ignore & IGN_Y) && r->y != n->y) report(h, "Y", r->y, n->y);
    if ((r->p ^ n->p) & pmask) report(h, "P", r->p, n->p);
    if (r->db != n->db) report(h, "DB", r->db, n->db);
    if (r->d != n->d) report(h, "D", r->d, n->d);
    for (uint32_t a = 0; a < sizeof s->wram; a++) {
        if (a >= 0x100 && a <= n->sp) continue;   /* dead stack */
        if (s->wram[a] != s_shadow.wram[a]) {
            char what[32];
            snprintf(what, sizeof what, "WRAM $%06X", 0x7E0000 + a);
            report(h, what, s_shadow.wram[a], s->wram[a]);
            break;
        }
    }
    if (memcmp(s->ppu, s_shadow.ppu, sizeof s->ppu)) report(h, "PPU registers", 0, 0);
    if (memcmp(s->dma, s_shadow.dma, sizeof s->dma)) report(h, "DMA registers", 0, 0);
    if (s->wram_addr != s_shadow.wram_addr) report(h, "$2181 address", s_shadow.wram_addr, s->wram_addr);
}

static int dispatch(Snes *s)
{
    Cpu *c = &s->cpu;
    uint32_t pc = (uint32_t)c->pb << 16 | c->pc;
    if (s_resume_fn && pc == s_resume_pc && c->sp == s_resume_sp) {
        void (*fn)(void) = s_resume_fn;
        s_resume_fn = NULL;
        Snes *ps = g_snes; Cpu *pc_ = g_cpu; uint8_t *pr = g_ram;
        g_snes = s; g_cpu = c; g_ram = s->wram;
        g_resuming = 1;
        fn();
        g_resuming = 0;
        g_snes = ps; g_cpu = pc_; g_ram = pr;
        return 1;
    }
    if (!s_at[pc & 0xFFFF]) return 0;
    int i;
    for (i = 0; i < g_nhooks; i++) if (g_hooks[i].addr == pc && s_enabled[i]) break;
    if (i == g_nhooks) return 0;
    const Hook *h = &g_hooks[i];
    if (c->e || c->d || (c->p & h->p_clear) || (~c->p & h->p_set)) {
        g_hook_fallbacks[i]++;
        return 0;
    }
    Snes *ps = g_snes; Cpu *pc_ = g_cpu; uint8_t *pr = g_ram;
    g_snes = s; g_cpu = c; g_ram = s->wram;
    g_hook_calls[i]++;
    g_last_hook = i;
    if (h->ignore & NO_SHADOW) { g_resuming = 0; h->fn(); }
    else if (g_hook_check && !s_depth) check_call(s, i);
    else { s_depth++; h->fn(); s_depth--; }
    g_snes = ps; g_cpu = pc_; g_ram = pr;
    return 1;
}

void hook_install(Snes *s, const uint8_t *enable)
{
    if (!s_enabled) {
        s_enabled = calloc(g_nhooks, 1);
        g_hook_calls = calloc(g_nhooks, sizeof *g_hook_calls);
        g_hook_fallbacks = calloc(g_nhooks, sizeof *g_hook_fallbacks);
    }
    for (int i = 0; i < g_nhooks; i++) {
        s_enabled[i] = enable ? enable[i] : 1;
        if (s_enabled[i]) s_at[g_hooks[i].addr & 0xFFFF] = 1;
    }
    s->hook = dispatch;
}
