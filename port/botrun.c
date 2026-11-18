/* Runs bot/bot.lua on the port instead of Mesen: the interpreter in
 * frame-step mode (one video frame = one logic frame, no lag), with the part
 * of Mesen's Lua API the bot uses (emu.read/write, callbacks, savestates).
 * Several times faster than Mesen and deterministic; the pad log the bot
 * writes (bot/inputlog.lua) replays in port/hybrid as usual.
 *
 * Usage: botrun <rom> <script.lua>   (env as for Mesen: SBM2_LOAD, ...)
 * Exit status: the script's emu.stop(code), 1 on a Lua error.
 *
 * Start states: SBM2_LOAD may be a Mesen savestate with a pad-log sidecar
 * (<state>.inputs, bot/inputlog.lua); the run then replays that log from
 * power-on to reach it, and the bot's emu.loadSavestate of the Mesen data
 * leaves the machine there. States the bot makes here are the port's own
 * (magic "SBM2PORT"), written to the same files (ckpt.mss, ...).
 *
 * Semantics kept from Mesen: endFrame runs at the end of a logic frame, then
 * inputPolled, then the pad is latched; an exec callback registered for any
 * address (the bot's onNextInstruction) runs right there, so a savestate made
 * in it is from before the latch, and loading one resumes after endFrame.
 * Not emulated: screenshots (empty), lag frames. CDL marks executed code
 * bytes only (flag $01, plus Mesen's M/X width bits on the opcode). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "path_pad.h"
#include "snes.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "../planner/agent_api.h"  /* (header only: agent_enemy_cells for the sims) */

extern int SANDBOX_ALLOW_LOADFILE;  /* Mesen's sandbox switch (lauxlib.c) */

static Snes m;
static lua_State *L;
static uint8_t *cdl;
/* Code/data logging per instruction (emu.getCdl*); off in agentrun unless
 * SBM2_CDL=1 (it costs ~a third of the live machine's time). */
static int cdl_on = 1;
static int stopped = -1;
static int loaded_state;       /* set by loadSavestate within a callback */
static int skip_end_frame;     /* the state was saved after its endFrame */
static int in_frame_cb;        /* inside on_frame (endFrame/inputPolled) */
static uint16_t live_pad;

#define INPUT_LEN (1u << 22)
static uint16_t *input;

/* --- callbacks --- */
enum { CB_READ, CB_WRITE, CB_EXEC };
typedef struct { int ref, type, live; uint32_t lo, hi; } MemCb;
#define MAX_CB 64
static MemCb cbs[MAX_CB];
static int ncb, nlive[3];
static uint32_t cb_lo[3], cb_hi[3];   /* bounds of the live ranges per type */

static uint8_t cb_read(void *ctx, uint32_t addr);
static void cb_write(void *ctx, uint32_t addr, uint8_t v);
static uint8_t (*bus_read)(void *, uint32_t);
static void (*bus_write)(void *, uint32_t, uint8_t);
/* The live machine's bus: the callback wrappers only while a read or write
 * callback is registered, else the core's own (its inlined fast path). */
static void cb_bounds(void)
{
    for (int t = 0; t < 3; t++) { cb_lo[t] = 0xFFFFFFFF; cb_hi[t] = 0; }
    for (int i = 0; i < ncb; i++) {
        if (!cbs[i].live) continue;
        int t = cbs[i].type;
        if (cbs[i].lo < cb_lo[t]) cb_lo[t] = cbs[i].lo;
        if (cbs[i].hi > cb_hi[t]) cb_hi[t] = cbs[i].hi;
    }
    if (bus_read) {
        m.cpu.read = cb_lo[CB_READ] <= cb_hi[CB_READ] ? cb_read : bus_read;
        m.cpu.write = cb_lo[CB_WRITE] <= cb_hi[CB_WRITE] ? cb_write : bus_write;
    }
}
enum { EV_END_FRAME, EV_INPUT_POLLED, EV_N };
static int ev_ref[EV_N][8], nev[EV_N];

static void lua_fail(void)
{
    fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    if (stopped < 0) stopped = 1;
}

static void call_ref(int ref, int nargs_addr, uint32_t addr, int value)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    int n = 0;
    if (nargs_addr) { lua_pushinteger(L, addr); lua_pushinteger(L, value); n = 2; }
    if (lua_pcall(L, n, 0, 0) != LUA_OK) lua_fail();
}

static void run_mem_cbs(int type, uint32_t addr, int value)
{
    for (int i = 0; i < ncb; i++)
        if (cbs[i].live && cbs[i].type == type && addr >= cbs[i].lo && addr <= cbs[i].hi)
            call_ref(cbs[i].ref, 1, addr, value);
}

static uint8_t (*bus_read)(void *, uint32_t);
static void (*bus_write)(void *, uint32_t, uint8_t);

static uint8_t cb_read(void *ctx, uint32_t addr)
{
    uint8_t v = bus_read(ctx, addr);
    if (addr >= cb_lo[CB_READ] && addr <= cb_hi[CB_READ]) run_mem_cbs(CB_READ, addr, v);
    return v;
}

static void cb_write(void *ctx, uint32_t addr, uint8_t v)
{
    bus_write(ctx, addr, v);
    if (addr >= cb_lo[CB_WRITE] && addr <= cb_hi[CB_WRITE]) run_mem_cbs(CB_WRITE, addr, v);
}

static uint32_t rom_offset(uint32_t addr)
{
    uint32_t o = addr & 0x3FFFFF;
    while (o >= m.rom_size) o -= m.rom_size > 0x200000 ? 0x200000 : m.rom_size;
    return o;
}

static int is_rom(uint32_t addr)
{
    uint8_t b = addr >> 16;
    return (b & 0xFE) != 0x7E && ((b & 0x40) || (addr & 0x8000));
}

static int vb_pending;
static void vblank_run(void);
static uint16_t *load_words(const char *path, uint32_t *n);
#ifdef SBM2_AGENT
/* agentrun without a script: the C driver (cd_*, below) plays the agent */
static int cdrv;
static void cd_frame(void);
static void cd_exec(uint32_t pc);
#endif
static int step_hook(Snes *s)
{
    if (vb_pending) {
        vb_pending = 0;
        vblank_run();
        if (loaded_state || stopped >= 0) return 1;
    }
    uint32_t pc = (uint32_t)s->cpu.pb << 16 | s->cpu.pc;
#ifdef SBM2_AGENT
    if (cdrv) cd_exec(pc);
#endif
    if (cdl_on && is_rom(pc)) {
        uint32_t o = rom_offset(pc);
        cdl[o] |= 0x01 | (s->cpu.p & 0x20 ? 0x20 : 0) | (s->cpu.p & 0x10 ? 0x10 : 0);
    }
    if (pc >= cb_lo[CB_EXEC] && pc <= cb_hi[CB_EXEC]) {
        loaded_state = 0;
        run_mem_cbs(CB_EXEC, pc, 0);
        if (loaded_state) return 1;   /* resume from the loaded state */
    }
    return stopped >= 0;              /* stop: run no more instructions */
}

static int frame_cb(Snes *s)
{
    (void)s;
    in_frame_cb = 1;
    loaded_state = 0;
    if (skip_end_frame) skip_end_frame = 0;
    else {
        for (int i = 0; i < nev[EV_END_FRAME]; i++) call_ref(ev_ref[EV_END_FRAME][i], 0, 0, 0);
        /* onNextInstruction: the next instruction is this one */
        if (nlive[CB_EXEC]) {
            uint32_t pc = (uint32_t)m.cpu.pb << 16 | m.cpu.pc;
            run_mem_cbs(CB_EXEC, pc, 0);
        }
    }
    in_frame_cb = 0;
    if (loaded_state || stopped >= 0) return 1;
    for (int i = 0; i < nev[EV_INPUT_POLLED]; i++) call_ref(ev_ref[EV_INPUT_POLLED][i], 0, 0, 0);
    input[m.frame % INPUT_LEN] = live_pad;
    return 0;
}

/* Timed mode: endFrame and inputPolled at each vblank, as in Mesen (lag
 * frames included); savestates made by exec callbacks come after it. */
/* Timed mode: the vblank line start (snes.c line_start) happens inside a
 * CPU instruction, so the Lua callbacks are run at the next instruction
 * boundary (step_hook): the machine is then whole (emu.simulate copies it,
 * and savestates are taken there anyway). The auto-joypad read has latched
 * input[frame] by then; the game reads it later (in its NMI handler), so
 * the latch is set again from the pad the callbacks chose. */
static void vblank_cb(Snes *s)
{
    (void)s;
    vb_pending = 1;
}

static void vblank_run(void)
{
    loaded_state = 0;
    for (int i = 0; i < nev[EV_END_FRAME]; i++) call_ref(ev_ref[EV_END_FRAME][i], 0, 0, 0);
    for (int i = 0; i < nev[EV_INPUT_POLLED]; i++) call_ref(ev_ref[EV_INPUT_POLLED][i], 0, 0, 0);
#ifdef SBM2_AGENT
    if (cdrv) cd_frame();
#endif
    input[m.frame % INPUT_LEN] = live_pad;
    if (m.nmitimen & 0x01) m.joy[0] = live_pad;
}

/* --- savestates --- */
static const char MAGIC[8] = "SBM2PORT";

static void push_state(lua_State *Ls)
{
    luaL_Buffer b;
    luaL_buffinit(Ls, &b);
    luaL_addlstring(&b, MAGIC, 8);
    uint8_t flag = in_frame_cb;
    luaL_addlstring(&b, (const char *)&flag, 1);
    luaL_addlstring(&b, (const char *)&m, sizeof m);
    if (m.sram_size) luaL_addlstring(&b, (const char *)m.sram, m.sram_size);
    luaL_pushresult(&b);
}

static int restore_state(const char *d, size_t n)
{
    if (n < 9 + sizeof m || memcmp(d, MAGIC, 8)) return 0;
    Snes keep = m;
    memcpy(&m, d + 9, sizeof m);
    /* pointers and buffers belong to this process */
    m.rom = keep.rom; m.rom_size = keep.rom_size;
    m.sram = keep.sram; m.sram_size = keep.sram_size;
    m.ppu_state = keep.ppu_state;
    m.input = keep.input; m.input_len = keep.input_len;
    m.pads = keep.pads; m.npads = keep.npads; m.pad_pos = keep.pad_pos;
    m.pad_rec = keep.pad_rec; m.pad_rec_len = keep.pad_rec_len; m.pad_rec_cap = keep.pad_rec_cap;
    m.pad_record = keep.pad_record;
    m.wait_at = keep.wait_at;
    m.hook = keep.hook; m.on_frame = keep.on_frame; m.on_vblank = keep.on_vblank; m.prof = keep.prof;
    m.framestep = keep.framestep;
    m.cpu.ctx = keep.cpu.ctx; m.cpu.read = keep.cpu.read; m.cpu.write = keep.cpu.write;
    m.cpu.idle = keep.cpu.idle;
    if (m.sram_size && n >= 9 + sizeof m + m.sram_size) memcpy(m.sram, d + 9 + sizeof m, m.sram_size);
    skip_end_frame = d[8];
    return 1;
}

/* The Mesen start state: we are already there (replayed its pad log), so
 * the first load of it is a no-op; later loads go back to that point. */
static char *start_state;
static size_t start_len;

/* --- the emu table --- */
enum { MT_CPU, MT_WRAM, MT_PRG, MT_N };

static int peek(int type, uint32_t a)
{
    switch (type) {
    case MT_WRAM: return m.wram[a & 0x1FFFF];
    case MT_PRG: return a < m.rom_size ? m.rom[a] : 0;
    default: {
        a &= 0xFFFFFF;
        uint8_t b = a >> 16;
        uint16_t lo = a;
        if (!(b & 0x40) && lo >= 0x4218 && lo < 0x4220) {
            uint16_t j = m.joy[(lo - 0x4218) >> 1];
            return lo & 1 ? j >> 8 : j & 0xFF;
        }
        return snes_peek(&m, a);
    }
    }
}

static void poke(int type, uint32_t a, uint8_t v)
{
    switch (type) {
    case MT_WRAM: m.wram[a & 0x1FFFF] = v; break;
    case MT_PRG: break;
    default: {
        uint8_t b = (a >> 16) & 0xFF;
        if ((b & 0xFE) == 0x7E) m.wram[a & 0x1FFFF] = v;
        else if (!(b & 0x40) && (a & 0xFFFF) < 0x2000) m.wram[a & 0x1FFF] = v;
    }
    }
}

static int l_read(lua_State *Ls)
{
    uint32_t a = luaL_checkinteger(Ls, 1);
    int t = luaL_optinteger(Ls, 2, MT_CPU), sgn = lua_toboolean(Ls, 3);
    int v = peek(t, a);
    lua_pushinteger(Ls, sgn && v >= 0x80 ? v - 0x100 : v);
    return 1;
}

static int l_read16(lua_State *Ls)
{
    uint32_t a = luaL_checkinteger(Ls, 1);
    int t = luaL_optinteger(Ls, 2, MT_CPU), sgn = lua_toboolean(Ls, 3);
    int v = peek(t, a) | peek(t, a + 1) << 8;
    lua_pushinteger(Ls, sgn && v >= 0x8000 ? v - 0x10000 : v);
    return 1;
}

/* emu.readBytes(addr, len, type): len bytes as a string (not in Mesen). */
static int l_readBytes(lua_State *Ls)
{
    uint32_t a = luaL_checkinteger(Ls, 1);
    lua_Integer n = luaL_checkinteger(Ls, 2);
    int t = luaL_optinteger(Ls, 3, MT_CPU);
    if (n < 0 || n > 0x100000) return luaL_error(Ls, "bad length");
    luaL_Buffer b;
    char *d = luaL_buffinitsize(Ls, &b, n);
    if (t == MT_WRAM && (a & 0x1FFFF) + n <= 0x20000) memcpy(d, m.wram + (a & 0x1FFFF), n);
    else for (lua_Integer i = 0; i < n; i++) d[i] = peek(t, a + i);
    luaL_pushresultsize(&b, n);
    return 1;
}

static unsigned live_gen;   /* bumped when the live machine is changed from outside */
static int l_write(lua_State *Ls)
{
    live_gen++;
    poke(luaL_optinteger(Ls, 3, MT_CPU), luaL_checkinteger(Ls, 1), luaL_checkinteger(Ls, 2));
    return 0;
}

static int l_write16(lua_State *Ls)
{
    uint32_t a = luaL_checkinteger(Ls, 1);
    int v = luaL_checkinteger(Ls, 2), t = luaL_optinteger(Ls, 3, MT_CPU);
    live_gen++;
    poke(t, a, v); poke(t, a + 1, v >> 8);
    return 0;
}

static int l_getMemorySize(lua_State *Ls)
{
    int t = luaL_checkinteger(Ls, 1);
    lua_pushinteger(Ls, t == MT_PRG ? (lua_Integer)m.rom_size : t == MT_WRAM ? 0x20000 : 0x1000000);
    return 1;
}

static int l_addMemoryCallback(lua_State *Ls)
{
    luaL_checktype(Ls, 1, LUA_TFUNCTION);
    int type = luaL_checkinteger(Ls, 2);
    uint32_t lo = luaL_checkinteger(Ls, 3), hi = luaL_optinteger(Ls, 4, lo);
    int i;
    for (i = 0; i < ncb && cbs[i].live; i++) {}
    if (i == ncb) { if (ncb == MAX_CB) return luaL_error(Ls, "too many callbacks"); ncb++; }
    lua_pushvalue(Ls, 1);
    cbs[i].ref = luaL_ref(Ls, LUA_REGISTRYINDEX);
    cbs[i].type = type; cbs[i].lo = lo; cbs[i].hi = hi; cbs[i].live = 1;
    nlive[type]++;
    cb_bounds();
    lua_pushinteger(Ls, i + 1);
    return 1;
}

static int l_removeMemoryCallback(lua_State *Ls)
{
    int i = luaL_checkinteger(Ls, 1) - 1;
    if (i >= 0 && i < ncb && cbs[i].live) {
        cbs[i].live = 0;
        nlive[cbs[i].type]--;
        luaL_unref(Ls, LUA_REGISTRYINDEX, cbs[i].ref);
        cb_bounds();
    }
    return 0;
}

static int l_addEventCallback(lua_State *Ls)
{
    luaL_checktype(Ls, 1, LUA_TFUNCTION);
    int t = luaL_checkinteger(Ls, 2);
    if (t < 0 || t >= EV_N || nev[t] == 8) return luaL_error(Ls, "unsupported event %d", t);
    lua_pushvalue(Ls, 1);
    ev_ref[t][nev[t]++] = luaL_ref(Ls, LUA_REGISTRYINDEX);
    lua_pushinteger(Ls, 1000 + t);
    return 1;
}

/* emu.simulate(pads, frames): lookahead on a scratch copy of the machine.
 * pads is an array of pad bitmasks (emu.padBits), one per frame (the last
 * one repeats); runs `frames` frames with no Lua callbacks and returns the
 * frame offset at which P1 dies ($7E8052 == $0801, C0:A675), or -1. The
 * real machine is untouched (the future input[] slots it writes are set
 * again by the live frames before they are used). */
#define SIM_HASH_D 40
/* One run ahead on a private copy (see l_simulate); thread-safe: the core
 * keeps no global state, and each job has its own machine, input and SRAM. */
typedef struct SimJob {
    Snes s;
    uint16_t *buf;
    uint8_t *sram;
    int frames, dead;
    /* closed-loop path (after the pad prefix, if any): pixel waypoints */
    int16_t *wp;
    int nwp, npre;
    int det;        /* > 0: B (remote detonation) held from this frame on for 4 */
    /* after the path: `post` pads (a bomb press), then a second path */
    uint16_t *post;
    int npost;
    int16_t *wp2;
    int nwp2;
    int ph, p0, at2;  /* phase: 1 path, 2 post, 3 path2, 4 B (remote) */
    int detEnd;       /* press B (4 frames, every 20) this many times once path2 is done */
    int detWait;      /* ... after standing this many frames in cover */
    int sx, sy, still; /* path_pad: closest to the waypoint so far, frames since, waypoint */
    uint64_t map0;     /* TileMap hash at the start ... */
    int mapChanged;    /* ... and whether it ever differed (sampled every 4 frames) */
    /* the result cache (sim_cache_*): resume from a cached end state */
    int resume, done;
    uint32_t f0;
    uint8_t h0;
    uint64_t hash[SIM_HASH_D + 1];   /* state hash at offsets 1..SIM_HASH_D (0: unknown) */
    int16_t *trace;   /* the agent's runs: P1 world x, y where the live
                       * frame callback would run, per frame offset */
    int tcap;
    int16_t *etrace;  /* AG_ESLOTS per frame offset: each enemy slot's cell (agent_enemy_cell) */
    int16_t *btrace;  /* 2 * nboss per frame offset: boss x, y (agent_boss_xy) */
    uint16_t bossBase[32];
    int nboss;
} SimJob;

/* The path controller: port/path_pad.h (unit tested from planner/test). */
static uint16_t path_pad(const Snes *s, const int16_t *wp, int n, int *at, int *mem)
{
    return path_pad_w(s->wram, wp, n, at, mem);
}

static void sim_setup(SimJob *j, const uint16_t *seq, int np, int frames)
{
    memcpy(&j->s, &m, sizeof m);
    Snes *s = &j->s;
    s->cpu.ctx = s;
    s->cpu.read = bus_read ? bus_read : m.cpu.read;
    s->cpu.write = bus_write ? bus_write : m.cpu.write;
    s->hook = NULL;
    s->on_vblank = NULL;
    s->on_frame = NULL;
    s->pads = NULL;
    s->pad_record = 0;
    s->prof = NULL;
    s->ppu_state = NULL;
    if (m.sram_size) {
        if (!j->sram) j->sram = malloc(m.sram_size);
        memcpy(j->sram, m.sram, m.sram_size);
        s->sram = j->sram;
    }
    j->buf = realloc(j->buf, (size_t)(frames + 3) * 2);
    uint16_t last = 0;
    for (int i = 0; i <= frames + 2; i++) {
        if (i < np) last = seq[i];
        j->buf[i] = last;
    }
    /* buf[i] = input[f0 + i]: the copy is taken in the frame callback
     * (vblank_run), whose pad is this frame's; the pads start there, as
     * the bot's own input would */
    s->input = j->buf - m.frame;
    s->input_len = m.frame + frames + 3;
    j->frames = frames;
}

/* the pad for buf[i] in the closed-loop part (path, post pads, path2) */
static uint16_t sim_pad(SimJob *j, const Snes *s, int i, int *at)
{
    if (j->ph == 1) {
        uint16_t p = path_pad(s, j->wp, j->nwp, at, &j->sx);
        /* (0 with waypoints left: let go in a jump's flight, not the end) */
        if (p || *at < j->nwp || !(j->npost || j->nwp2)) return p;
        j->ph = 2; j->p0 = i;
    }
    if (j->ph == 2) {
        if (i - j->p0 < j->npost) return j->post[i - j->p0];
        j->ph = 3;
    }
    if (j->ph == 3) {
        uint16_t p = path_pad(s, j->wp2, j->nwp2, &j->at2, &j->sx);
        if (p || j->at2 < j->nwp2 || !j->detEnd) return p;
        j->ph = 4; j->p0 = i + j->detWait;
    }
    /* detEnd presses, 20 frames apart: each sets off the oldest remote
     * bomb (docs/mechanics.md 10), so older ones (traps) go first */
    { int k = i - j->p0; return k >= 0 && k < 20 * j->detEnd && k % 20 < 4 ? PAD_B : 0; }
}

/* A hit that costs a heart (P1 +$EF down) counts as a death in the sim:
 * with hearts in hand a hit is no death, and the shield would let every
 * one through (3-7, run BH s0: fled into a blast with 3 hearts, no
 * override). emu.simHearts(false) turns it off (a trap crossed on purpose). */
static int sim_heart_death = 1;
static int l_simHearts(lua_State *Ls) { sim_heart_death = lua_toboolean(Ls, 1); return 0; }

/* The machine at a frame start, as a hash: WRAM, the CPU's registers, PPU
 * and DMA registers and the timing/APU fields (not the pads latched: a run
 * ahead latches its own). Equal hashes = the same machine (for the cache). */
static uint64_t mix64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, b + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
static uint64_t state_hash(const Snes *s)
{
    uint64_t h = 0xCBF29CE484222325ull;
    h = mix64(h, s->wram, sizeof s->wram);
    h = mix64(h, &s->cpu, offsetof(Cpu, ctx));
    h = mix64(h, s->ppu, sizeof s->ppu);
    h = mix64(h, &s->mdr, 1);
    size_t a = offsetof(Snes, clock), b = offsetof(Snes, input);
    uint8_t tmp[offsetof(Snes, input) - offsetof(Snes, clock)];
    memcpy(tmp, (const uint8_t *)s + a, b - a);
    memset(tmp + offsetof(Snes, joy) - a, 0, sizeof s->joy);
    h = mix64(h, tmp, b - a);
    return h | 1;
}

static void *sim_run(void *arg)
{
    SimJob *j = arg;
    Snes *s = &j->s;
    uint32_t f0 = j->resume ? j->f0 : s->frame;
    uint8_t h0 = j->resume ? j->h0 : s->wram[0x80EF];
    j->dead = -1;
    long guard = 0;
    int at = 0;
    j->ph = 1; j->at2 = 0;
    j->sx = 1 << 20; j->sy = 0; j->still = -1;
    if (!j->resume) {
        j->map0 = agent_map_sig(s->wram);
        j->mapChanged = 0;
        memset(j->hash, 0, sizeof j->hash);
        if (j->nwp && j->npre == 0) j->buf[0] = sim_pad(j, s, 0, &at);
        /* this frame's pad: latched already (see vblank_cb) */
        if (s->nmitimen & 0x01) s->joy[0] = j->buf[0];
    }
    /* A death locks P1 ($52 & $F809 == $0801) until the lives go down
     * (200+ frames); a jump pad's throw, a carry or a warp locks it the
     * same way but lets go within ~75. So a lock is a death only if it
     * still holds 90 frames on (the death is when it began); the run goes
     * past the horizon to tell. On the exit cell ($80A8 == $5D72) it is
     * the warp out: -2. */
    uint32_t lockAt = 0;
    int hpend = 0;   /* a frame offset whose hash is due (at the live callback's point) */
    int tpend = 0;   /* ... whose trace entry is due */
    if (j->trace && j->tcap > 0) {
        j->trace[0] = (int16_t)((s->wram[0x8010] | s->wram[0x8011] << 8) + (s->wram[0x5F01] | s->wram[0x5F02] << 8));
        j->trace[1] = (int16_t)((s->wram[0x8018] | s->wram[0x8019] << 8) + (s->wram[0x5EFB] | s->wram[0x5EFC] << 8));
        if (j->etrace) agent_enemy_cells(s->wram, j->etrace);
        if (j->btrace) for (int b = 0; b < j->nboss; b++) agent_boss_xy(s->wram, j->bossBase[b], &j->btrace[2 * b], &j->btrace[2 * b + 1]);
    }
    while ((s->frame < f0 + (uint32_t)j->frames || lockAt) && guard++ < 400000000L) {
        uint32_t fr = s->frame;
        /* where step_hook would run vblank_run on the live machine: the
         * first instruction boundary after the frame start with no NMI
         * being taken (see snes_step) */
        if ((hpend || tpend) && !(s->nmi_pending && s->nmi_at <= s->cycle_start) && !s->cpu.nmi && !s->cpu.wai && !s->cpu.stp) {
            if (hpend) j->hash[hpend] = state_hash(s);
            if (tpend) {
                j->trace[2 * tpend] = (int16_t)((s->wram[0x8010] | s->wram[0x8011] << 8) + (s->wram[0x5F01] | s->wram[0x5F02] << 8));
                j->trace[2 * tpend + 1] = (int16_t)((s->wram[0x8018] | s->wram[0x8019] << 8) + (s->wram[0x5EFB] | s->wram[0x5EFC] << 8));
                if (j->etrace) agent_enemy_cells(s->wram, j->etrace + AG_ESLOTS * tpend);
                if (j->btrace)
                    for (int b = 0; b < j->nboss; b++) {
                        int16_t *q = j->btrace + 2 * (j->nboss * tpend + b);
                        agent_boss_xy(s->wram, j->bossBase[b], &q[0], &q[1]);
                    }
            }
            hpend = tpend = 0;
        }
        snes_step(s);
        if (s->frame != fr) {
            /* input[F + 1] (buf[F + 1 - f0]) is read when frame F + 1 begins */
            int i = (int)(s->frame - f0) + 1;
            hpend = !j->resume && i - 1 <= SIM_HASH_D ? i - 1 : 0;
            tpend = j->trace && i - 1 < j->tcap ? i - 1 : 0;
            if (j->nwp && i >= j->npre && i <= j->frames + 2) j->buf[i] = sim_pad(j, s, i, &at);
            if (j->det && i >= j->det && i < j->det + 4 && i <= j->frames + 2) j->buf[i] |= PAD_B;
            /* (world 2 soft blocks grow back within a run: a change seen on
             * the way counts, not just the end state) */
            if (!j->mapChanged && (i & 3) == 0 && agent_map_sig(s->wram) != j->map0) j->mapChanged = 1;
            uint16_t lock = s->wram[0x8052] | s->wram[0x8053] << 8;
            if (sim_heart_death && s->wram[0x80EF] < h0) {
                uint16_t at8 = s->wram[0x80A8] | s->wram[0x80A9] << 8;
                uint16_t ex = s->wram[0x5D72] | s->wram[0x5D73] << 8;
                if (at8 == ex) { j->dead = -2; break; }  /* (the stage change resets them) */
                j->dead = (int)(s->frame - f0); break;
            }
            if (s->wram[0x80EF] > h0) h0 = s->wram[0x80EF];
            if ((lock & 0xF809) == 0x0801 && !p1_on_pad(s->wram)) {
                uint16_t at8 = s->wram[0x80A8] | s->wram[0x80A9] << 8;
                uint16_t ex = s->wram[0x5D72] | s->wram[0x5D73] << 8;
                if (at8 == ex) { j->dead = -2; break; }
                if (!lockAt) lockAt = s->frame;
                else if (s->frame - lockAt >= 90) { j->dead = (int)(lockAt - f0); break; }
            } else lockAt = 0;  /* (a jump: $1801 in the air, $0809 landing, $0801 on a pad) */
        }
    }
    j->h0 = h0;
    return NULL;
}

static uint16_t *lua_seq(lua_State *Ls, int idx, int *np)
{
    int n = (int)lua_rawlen(Ls, idx);
    uint16_t *q = malloc((size_t)(n ? n : 1) * 2);
    for (int i = 0; i < n; i++) { lua_rawgeti(Ls, idx, i + 1); q[i] = (uint16_t)lua_tointeger(Ls, -1); lua_pop(Ls, 1); }
    *np = n;
    return q;
}

/* emu.simulate(pads, frames): pads is an array of pad bitmasks
 * (emu.padBits), one per frame (the last one repeats); runs `frames` frames
 * on a copy of the machine with no Lua callbacks and returns the frame
 * offset at which P1 dies ($7E8052 == $0801, C0:A675), or -1 (-2: it
 * left by the exit). The real
 * machine is untouched. */
#define SIM_THREADS 4
static SimJob *jobs[SIM_THREADS];
static int sim_ret(lua_State *Ls, SimJob *j)
{
    lua_pushinteger(Ls, j->dead);
    lua_pushinteger(Ls, j->s.wram[0x8010] | j->s.wram[0x8011] << 8);
    lua_pushinteger(Ls, j->s.wram[0x8018] | j->s.wram[0x8019] << 8);
    lua_pushinteger(Ls, j->s.frame);
    lua_pushinteger(Ls, j->s.wram[0x8052] | j->s.wram[0x8053] << 8);
    lua_pushinteger(Ls, j->s.wram[0x80A8] | j->s.wram[0x80A9] << 8);
    lua_pushinteger(Ls, j->s.wram[0x5D72] | j->s.wram[0x5D73] << 8);
    return 7;
}
static int l_simulate(lua_State *Ls)
{
    luaL_checktype(Ls, 1, LUA_TTABLE);
    int frames = (int)luaL_checkinteger(Ls, 2);
    int np;
    uint16_t *q = lua_seq(Ls, 1, &np);
    if (!jobs[0]) jobs[0] = calloc(1, sizeof(SimJob));
    sim_setup(jobs[0], q, np, frames);
    jobs[0]->nwp = jobs[0]->npre = jobs[0]->det = jobs[0]->npost = jobs[0]->nwp2 = jobs[0]->detEnd = jobs[0]->detWait = 0;
    jobs[0]->resume = 0;
    free(q);
    sim_run(jobs[0]);
    return sim_ret(Ls, jobs[0]);
}

/* emu.simulateMany({pads, pads, ...}, frames [, stopOnLive [, detAt]]): the same for
 * each sequence, SIM_THREADS at a time in threads; returns an array of
 * death offsets (-1 = lives). With stopOnLive, batches after the first one
 * holding a sequence that lives are skipped (their entries are nil). With
 * detAt, B (the remote detonation) is pressed at that frame for 4 frames
 * in every sequence. An entry may also be { path = {x1, y1, ...},
 * pre = pads }: the pads, then the simulator steers along the pixel
 * waypoints (path_pad). */
#include <pthread.h>
/* The results of one run, kept for the cache. */
typedef struct SimRes { int dead, fl, en, hp, reach, ph, px, py; } SimRes;

static void sim_result(const SimJob *j, SimRes *r)
{
    const uint8_t *w = j->s.wram;
    r->dead = j->dead;
    int fl = 0;
    for (int o = 0; o < 32 * 0x80; o += 2) fl += w[0x1C000 + o] == 0;
    r->fl = fl;
    int en = 0, hp = 0;
#ifdef SBM2_AGENT
    agent_enemies(w, &en, &hp);  /* (the agent's count: $98 too, planner/agent_api.h) */
#else
    for (int base = 0x8400; base <= 0x8F00; base += 0x100) {
        int hi = w[base + 1] & 0xF0, ty = w[base + 0x7E];
        if ((hi == 0x90 || hi == 0xA0) && !(w[base + 5] & 0x80) && ty < 0xC0 && ty != 0x90 && ty != 0x98) {
            en++;
            hp += w[base + 0x43] & 0x7F;
        }
    }
#endif
    r->en = en; r->hp = hp;
    /* (world pixels: + the scroll origin, as path_pad) */
    r->ph = j->ph;
    r->px = (int)(uint16_t)((w[0x8010] | w[0x8011] << 8) + (w[0x5F01] | w[0x5F02] << 8));
    r->py = (int)(uint16_t)((w[0x8018] | w[0x8019] << 8) + (w[0x5EFB] | w[0x5EFC] << 8));
    int px = r->px / 16, py = r->py / 16, reach = 0;
    if (px >= 0 && px < 32 && py >= 0 && py < 32) {
        uint8_t seen[32 * 32] = {0};
        int q[32 * 32], qh = 0, qt = 0;
        seen[py * 32 + px] = 1; q[qt++] = py * 32 + px;
        while (qh < qt && reach < 64) {
            int k = q[qh++], c = k % 32, rr = k / 32;
            reach++;
            static const int dc[4] = {1, -1, 0, 0}, dr[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; d++) {
                int nc = c + dc[d], nr = rr + dr[d];
                if (nc < 0 || nc >= 32 || nr < 0 || nr >= 32) continue;
                int nk = nr * 32 + nc;
                if (seen[nk] || (w[0x1C000 + nr * 0x80 + nc * 2] & 0x80)) continue;
                seen[nk] = 1; q[qt++] = nk;
            }
        }
    }
    r->reach = reach;
}

/* --- the run-ahead cache ---
 * The lookahead asks for the same runs again: the same sequence from the
 * same frame (a list run again from its start), or the rest of a sequence
 * run a few frames ago that the machine has followed since (the bot held
 * its pad). A run from the same frame and machine (frame, clock, no write
 * or load since) with the same inputs gives the same results; and if the
 * live machine now hashes the same as a cached run did d frames in, the
 * rest of that run is this one's start: it goes on from the cached end
 * state for the frames it lacks. Same results either way, frame for frame.
 * SBM2_SIMCACHE=0 turns it off. */
#define SIM_CACHE_N 48
typedef struct SimEntry {
    int used;
    uint32_t F;              /* m.frame at the start */
    uint64_t clock0;
    unsigned gen;
    int heart;               /* sim_heart_death then */
    uint16_t *key; size_t nkey;   /* the inputs, see sim_key */
    SimRes res;
    /* a plain sequence that lived: its end state, to go on from */
    Snes *end; uint8_t *sram;
    uint16_t *buf; int frames;
    uint8_t h0;
    uint64_t hash[SIM_HASH_D + 1];
    unsigned long stamp;
} SimEntry;
static SimEntry sim_cache[SIM_CACHE_N];
static unsigned long sim_stamp;
static int sim_cache_on = -1;
static unsigned long sim_hits0, sim_hitsD, sim_runs;

/* The inputs of a job as one word string: frames, det, path lengths, pads,
 * waypoints, post pads, second path, detEnd/detWait. */
static uint16_t *sim_key(const SimJob *j, size_t *n)
{
    int nb = j->frames + 3;
    size_t len = 8 + (size_t)nb + 2 * j->nwp + j->npost + 2 * j->nwp2;
    uint16_t *k = malloc(len * 2), *p = k;
    *p++ = (uint16_t)j->frames; *p++ = (uint16_t)j->det; *p++ = (uint16_t)j->nwp; *p++ = (uint16_t)j->npre;
    *p++ = (uint16_t)j->npost; *p++ = (uint16_t)j->nwp2; *p++ = (uint16_t)j->detEnd; *p++ = (uint16_t)j->detWait;
    memcpy(p, j->buf, nb * 2); p += nb;
    if (j->nwp) { memcpy(p, j->wp, 4 * j->nwp); p += 2 * j->nwp; }
    if (j->npost) { memcpy(p, j->post, 2 * j->npost); p += j->npost; }
    if (j->nwp2) { memcpy(p, j->wp2, 4 * j->nwp2); p += 2 * j->nwp2; }
    *n = len;
    return k;
}

static uint64_t live_hash; static uint32_t live_hash_frame; static unsigned live_hash_gen; static uint64_t live_hash_clock;

/* Looks j up; on a hit fills res and returns 1 (done), or readies j to go
 * on from a cached end state and returns 2. */
static int sim_cache_find(SimJob *j, const uint16_t *key, size_t nkey, SimRes *res)
{
    for (int e = 0; e < SIM_CACHE_N; e++) {
        SimEntry *c = &sim_cache[e];
        if (!c->used || c->gen != live_gen || c->heart != sim_heart_death) continue;
        if (c->F == m.frame && c->clock0 == m.clock && c->nkey == nkey && !memcmp(c->key, key, nkey * 2)) {
            *res = c->res; c->stamp = ++sim_stamp; sim_hits0++;
            return 1;
        }
    }
    if (j->nwp) return 0;    /* (closed-loop paths: only the same run again) */
    for (int e = 0; e < SIM_CACHE_N; e++) {
        SimEntry *c = &sim_cache[e];
        if (!c->used || !c->end || c->gen != live_gen || c->heart != sim_heart_death) continue;
        uint32_t d = m.frame - c->F;
        if (m.frame <= c->F || d > SIM_HASH_D || !c->hash[d]) continue;
        int have = c->frames - (int)d;           /* frames of this run already run */
        if (have < 1 || have > j->frames) continue;
        /* the same pads from here on, as far as the cached run went */
        if (memcmp(j->buf, c->buf + d, (size_t)(have + 3) * 2)) continue;
        if (live_hash_frame != m.frame || live_hash_gen != live_gen || live_hash_clock != m.clock || !live_hash) {
            live_hash = state_hash(&m); live_hash_frame = m.frame; live_hash_gen = live_gen; live_hash_clock = m.clock;
        }
        if (live_hash != c->hash[d]) continue;
        /* go on from the cached end state */
        Snes *s = &j->s;
        uint16_t *buf = j->buf;
        uint8_t *sram = j->sram;
        memcpy(s, c->end, sizeof *s);
        s->cpu.ctx = s;
        if (c->sram && sram) { memcpy(sram, c->sram, m.sram_size); s->sram = sram; }
        s->input = buf - m.frame;
        s->input_len = m.frame + j->frames + 3;
        j->resume = 1; j->f0 = m.frame; j->h0 = c->h0;
        memset(j->hash, 0, sizeof j->hash);
        for (int k = 1; k + (int)d <= SIM_HASH_D; k++) j->hash[k] = c->hash[k + d];
        c->stamp = ++sim_stamp; sim_hitsD++;
        return 2;
    }
    return 0;
}

static void sim_cache_put(const SimJob *j, uint16_t *key, size_t nkey, const SimRes *res)
{
    int v = 0;
    for (int e = 0; e < SIM_CACHE_N; e++) {
        if (!sim_cache[e].used) { v = e; break; }
        if (sim_cache[e].stamp < sim_cache[v].stamp) v = e;
    }
    SimEntry *c = &sim_cache[v];
    free(c->key);
    c->used = 1; c->F = m.frame; c->clock0 = m.clock; c->gen = live_gen; c->heart = sim_heart_death;
    c->key = key; c->nkey = nkey; c->res = *res; c->stamp = ++sim_stamp;
    if (!j->nwp && j->dead == -1) {
        if (!c->end) c->end = malloc(sizeof(Snes));
        memcpy(c->end, &j->s, sizeof(Snes));
        if (m.sram_size) {
            if (!c->sram) c->sram = malloc(m.sram_size);
            memcpy(c->sram, j->s.sram, m.sram_size);
        }
        c->buf = realloc(c->buf, (size_t)(j->frames + 3) * 2);
        memcpy(c->buf, j->buf, (size_t)(j->frames + 3) * 2);
        c->frames = j->frames;
        c->h0 = j->h0;
        memcpy(c->hash, j->hash, sizeof c->hash);
    } else {
        free(c->end); c->end = NULL;
    }
}

static void push_res(lua_State *Ls, int out, int idx, const SimRes *r)
{
    lua_pushinteger(Ls, r->dead); lua_rawseti(Ls, out, idx);
    lua_pushinteger(Ls, r->fl); lua_rawseti(Ls, out + 1, idx);
    lua_pushinteger(Ls, r->en); lua_rawseti(Ls, out + 2, idx);
    lua_pushfstring(Ls, "%d:%d,%d", r->ph, r->px, r->py); lua_rawseti(Ls, out + 3, idx);
    lua_pushinteger(Ls, r->reach); lua_rawseti(Ls, out + 4, idx);
    lua_pushinteger(Ls, r->hp); lua_rawseti(Ls, out + 5, idx);
}

static int l_simulateMany(lua_State *Ls)
{
    luaL_checktype(Ls, 1, LUA_TTABLE);
    int frames = (int)luaL_checkinteger(Ls, 2);
    int stop = lua_toboolean(Ls, 3);
    int det = (int)luaL_optinteger(Ls, 4, 0);
    int n = (int)lua_rawlen(Ls, 1);
    if (sim_cache_on < 0) { const char *e = getenv("SBM2_SIMCACHE"); sim_cache_on = !(e && *e == '0'); }
    /* results: death offsets (-1 = lives); floor tiles ($00) in the map at
     * the end; live enemies at the end (not mounts $90/$98, not gadgets
     * $C0+); "phase:x,y" at the end, world pixels; cells P1 can walk to at
     * the end (tile bit 7 clear; up to 64); the live enemies' HP (+$43 &
     * $7F) summed, at the end */
    for (int t = 0; t < 6; t++) lua_createtable(Ls, n, 0);
    int out = lua_gettop(Ls) - 5;
    for (int b = 0; b < n; b += SIM_THREADS) {
        int k = n - b < SIM_THREADS ? n - b : SIM_THREADS;
        pthread_t th[SIM_THREADS];
        uint16_t *keys[SIM_THREADS];
        size_t nkeys[SIM_THREADS];
        SimRes res[SIM_THREADS];
        int run[SIM_THREADS], nrun = 0;
        for (int i = 0; i < k; i++) {
            if (!jobs[i]) jobs[i] = calloc(1, sizeof(SimJob));
            lua_rawgeti(Ls, 1, b + i + 1);
            int e = lua_gettop(Ls), np, nw = 0;
            lua_getfield(Ls, e, "path");
            if (lua_istable(Ls, -1)) {
                nw = (int)lua_rawlen(Ls, -1) / 2;
                jobs[i]->wp = realloc(jobs[i]->wp, (size_t)(nw ? nw : 1) * 4);
                for (int w = 0; w < 2 * nw; w++) { lua_rawgeti(Ls, -1, w + 1); jobs[i]->wp[w] = (int16_t)lua_tointeger(Ls, -1); lua_pop(Ls, 1); }
                lua_pop(Ls, 1);
                jobs[i]->npost = jobs[i]->nwp2 = 0;
                lua_getfield(Ls, e, "detEnd");
                jobs[i]->detEnd = lua_toboolean(Ls, -1);
                lua_pop(Ls, 1);
                lua_getfield(Ls, e, "detWait");
                jobs[i]->detWait = (int)lua_tointeger(Ls, -1);
                lua_pop(Ls, 1);
                lua_getfield(Ls, e, "post");
                if (lua_istable(Ls, -1)) {
                    int npo;
                    uint16_t *po = lua_seq(Ls, lua_gettop(Ls), &npo);
                    free(jobs[i]->post);
                    jobs[i]->post = po;
                    jobs[i]->npost = npo;
                }
                lua_pop(Ls, 1);
                lua_getfield(Ls, e, "path2");
                if (lua_istable(Ls, -1)) {
                    int n2 = (int)lua_rawlen(Ls, -1) / 2;
                    jobs[i]->wp2 = realloc(jobs[i]->wp2, (size_t)(n2 ? n2 : 1) * 4);
                    for (int w = 0; w < 2 * n2; w++) { lua_rawgeti(Ls, -1, w + 1); jobs[i]->wp2[w] = (int16_t)lua_tointeger(Ls, -1); lua_pop(Ls, 1); }
                    jobs[i]->nwp2 = n2;
                }
                lua_pop(Ls, 1);
                lua_getfield(Ls, e, "pre");
                if (!lua_istable(Ls, -1)) { lua_pop(Ls, 1); lua_newtable(Ls); }
            } else lua_pop(Ls, 1), lua_pushvalue(Ls, e), jobs[i]->npost = jobs[i]->nwp2 = jobs[i]->detEnd = jobs[i]->detWait = 0;
            uint16_t *q = lua_seq(Ls, lua_gettop(Ls), &np);
            lua_pop(Ls, 2);
            sim_setup(jobs[i], q, np, frames);
            jobs[i]->nwp = nw;
            jobs[i]->npre = nw ? np : 0;
            if (nw) for (int f = np; f <= frames + 2; f++) jobs[i]->buf[f] = 0;
            jobs[i]->det = det > 0 && det <= frames ? det : 0;
            if (jobs[i]->det) for (int f = det; f < det + 4; f++) jobs[i]->buf[f] |= PAD_B;
            free(q);
            jobs[i]->resume = 0;
            jobs[i]->done = 0;
            keys[i] = NULL;
            if (sim_cache_on) {
                keys[i] = sim_key(jobs[i], &nkeys[i]);
                if (sim_cache_find(jobs[i], keys[i], nkeys[i], &res[i]) == 1) jobs[i]->done = 1;
            }
            if (!jobs[i]->done) run[nrun++] = i;
        }
        for (int r = 1; r < nrun; r++) pthread_create(&th[r], NULL, sim_run, jobs[run[r]]);
        if (nrun) sim_run(jobs[run[0]]);
        for (int r = 1; r < nrun; r++) pthread_join(th[r], NULL);
        sim_runs += nrun;
        int live = 0;
        for (int i = 0; i < k; i++) {
            if (!jobs[i]->done) {
                sim_result(jobs[i], &res[i]);
                if (sim_cache_on) sim_cache_put(jobs[i], keys[i], nkeys[i], &res[i]);
            } else free(keys[i]);
            push_res(Ls, out, b + i + 1, &res[i]);
            if (res[i].dead < 0) live = 1;
        }
        if (stop && live) break;
    }
    return 6;
}

#ifdef SBM2_AGENT
/* --- the C++ agent (planner/agent.hpp): agent_call(frame) -> pad, log ---
 * Its oracle: runs on copies of the live machine, as emu.simulateMany's
 * path entries (no cache: the agent plays back the pads each run pressed,
 * so it needs them all). */
#include "agent_api.h"
static void *agent;
static double ag_tsim, ag_tagent; static unsigned long ag_nsim, ag_simfr, ag_calls;
static double ag_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* The oracle's disk cache (SBM2_AGCACHE=dir): a run's result keyed by the
 * live machine (state_hash), the request and the emulator's source
 * (SBM2_CORE_ID, from the Makefile: snes/cpu/ppu/botrun sources). The game
 * is deterministic, so a hit is the run itself: a rerun of an unchanged
 * scenario runs no sims, and after an agent change everything up to its
 * first different decision is a hit. */
#ifndef SBM2_CORE_ID
#define SBM2_CORE_ID "dev"
#endif
static const char *ag_cache;
static unsigned long ag_hits;
typedef struct { uint32_t magic; int32_t dead, en, hp, px, py, ph, cap; uint64_t mapSig; int32_t bossHp, nbt, mapChanged, emask; } AgCacheHdr;
#define AG_MAGIC 0x41474334
static uint64_t ag_key(uint64_t live, const AgSimReq *r, int cap)
{
    uint64_t h = mix64(live, SBM2_CORE_ID, strlen(SBM2_CORE_ID));
    int32_t v[8] = {r->npre, r->npath, r->npost, r->npath2, r->detEnd, r->frames, cap, sim_heart_death};
    h = mix64(h, v, sizeof v);
    if (r->npre) h = mix64(h, r->pre, (size_t)r->npre * 2);
    if (r->npath) h = mix64(h, r->path, (size_t)r->npath * 4);
    if (r->npost) h = mix64(h, r->post, (size_t)r->npost * 2);
    if (r->npath2) h = mix64(h, r->path2, (size_t)r->npath2 * 4);
    if (r->wantEnemies) h = mix64(h, "etrace", 6);
    if (r->nboss) { h = mix64(h, "boss", 4); h = mix64(h, r->bossBase, (size_t)r->nboss * 2); }
    if (r->wantBoss) h = mix64(h, "btrace", 6);
    return h;
}
static void ag_cache_path(char *b, size_t n, uint64_t k) { snprintf(b, n, "%s/%02x/%016llx", ag_cache, (unsigned)(k >> 56), (unsigned long long)k); }
static int ag_cache_get(uint64_t k, AgSimRes *o)
{
    char p[1024];
    ag_cache_path(p, sizeof p, k);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    AgCacheHdr h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == AG_MAGIC && h.cap == o->cap &&
             fread(o->pads, 2, (size_t)h.cap, f) == (size_t)h.cap && fread(o->trace, 2, (size_t)h.cap * 2, f) == (size_t)h.cap * 2 &&
             (!o->etrace || fread(o->etrace, 2, (size_t)h.cap * AG_ESLOTS, f) == (size_t)h.cap * AG_ESLOTS) &&
             (!o->btrace || fread(o->btrace, 2, (size_t)h.nbt, f) == (size_t)h.nbt);
    fclose(f);
    if (!ok) return 0;
    o->dead = h.dead; o->enemies = h.en; o->hp = h.hp; o->px = h.px; o->py = h.py; o->ph = h.ph; o->mapSig = h.mapSig; o->mapChanged = h.mapChanged;
    o->bossHp = h.bossHp;
    o->enemyMask = h.emask;
    return 1;
}
static void ag_cache_put(uint64_t k, const AgSimRes *o, int nboss)
{
    int nbt = o->btrace ? 2 * nboss * o->cap : 0;
    char p[1024], t[1100];
    ag_cache_path(p, sizeof p, k);
    snprintf(t, sizeof t, "%s/%02x", ag_cache, (unsigned)(k >> 56));
    mkdir(ag_cache, 0777);
    mkdir(t, 0777);
    snprintf(t, sizeof t, "%s.%d.tmp", p, (int)getpid());
    FILE *f = fopen(t, "wb");
    if (!f) return;
    AgCacheHdr h = {AG_MAGIC, o->dead, o->enemies, o->hp, o->px, o->py, o->ph, o->cap, o->mapSig, o->bossHp, nbt, o->mapChanged, o->enemyMask};
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(o->pads, 2, (size_t)o->cap, f) == (size_t)o->cap &&
             fwrite(o->trace, 2, (size_t)o->cap * 2, f) == (size_t)o->cap * 2 &&
             (!o->etrace || fwrite(o->etrace, 2, (size_t)o->cap * AG_ESLOTS, f) == (size_t)o->cap * AG_ESLOTS) &&
             (!o->btrace || fwrite(o->btrace, 2, (size_t)nbt, f) == (size_t)nbt);
    if (fclose(f) == 0 && ok) rename(t, p);
    else remove(t);
}

/* SBM2_OVERLAY=<file>: what the agent thinks, for build/port/play's
 * overlays (-O): text lines keyed by the video frame (m.frame):
 *   S <vf> <dead> <n> x y ... | k:pad ...
 *                               an oracle run: P1's world x, y every 2nd
 *                               frame (dead: frame offset, -1 lived, -2
 *                               exit), then its pads run-length encoded
 *   P <vf> <at> <n> x y ...     a new move: P1's planned path (every 2nd
 *                               frame from <at>)
 *   A <vf> x y                  a bomb press in that move, where
 *   W <vf> <what>               the move's kind
 *   L <vf> <text>               an agent log line */
static FILE *ov_f;
static int ov_open(void)
{
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("SBM2_OVERLAY");
        if (e && *e) ov_f = fopen(e, "w");
    }
    return ov_f != NULL;
}
static void ov_trace(const int16_t *t, int from, int to)
{
    for (int f = from; f < to; f += 2) fprintf(ov_f, " %d %d", t[2 * f], t[2 * f + 1]);
}

static void ag_run(const AgSimReq *rq, AgSimRes *rs, int n);
static void ag_sim(void *ctx, const AgSimReq *rq, AgSimRes *rs, int n)
{
    (void)ctx;
    double t0 = ag_now();
    static int init;
    if (!init) { init = 1; ag_cache = getenv("SBM2_AGCACHE"); if (ag_cache && !*ag_cache) ag_cache = NULL; }
    if (!ag_cache) { ag_run(rq, rs, n); goto done; }
    {
        uint64_t live = state_hash(&m);
        AgSimReq *mq = malloc(sizeof *mq * (size_t)n);
        AgSimRes *ms = malloc(sizeof *ms * (size_t)n);
        int *mi = malloc(sizeof *mi * (size_t)n), nm = 0;
        uint64_t *keys = malloc(sizeof *keys * (size_t)n);
        for (int i = 0; i < n; i++) {
            keys[i] = ag_key(live, &rq[i], rs[i].cap);
            if (ag_cache_get(keys[i], &rs[i])) { ag_hits++; continue; }
            mq[nm] = rq[i]; ms[nm] = rs[i]; mi[nm++] = i;
        }
        if (nm) ag_run(mq, ms, nm);
        for (int k = 0; k < nm; k++) { rs[mi[k]] = ms[k]; ag_cache_put(keys[mi[k]], &rs[mi[k]], rq[mi[k]].nboss); }
        free(mq); free(ms); free(mi); free(keys);
    }
done:
    if (ov_open())
        for (int i = 0; i < n; i++) {
            int len = rs[i].dead >= 0 ? rs[i].dead + 1 : rq[i].frames;
            if (len > rs[i].cap) len = rs[i].cap;
            fprintf(ov_f, "S %u %d %d", m.frame, rs[i].dead, (len + 1) / 2);
            ov_trace(rs[i].trace, 0, len);
            /* its pads, run-length: " | count:pad ..." (play reruns it) */
            int np = rq[i].frames + 3 < rs[i].cap ? rq[i].frames + 3 : rs[i].cap;
            fputs(" |", ov_f);
            for (int f = 0; f < np; ) {
                int k = f;
                while (k < np && rs[i].pads[k] == rs[i].pads[f]) k++;
                fprintf(ov_f, " %d:%x", k - f, rs[i].pads[f]);
                f = k;
            }
            fputc('\n', ov_f);
        }
    ag_nsim += (unsigned long)n;
    ag_tsim += ag_now() - t0;
    if (getenv("SBM2_AGPROF")) {
        fprintf(stderr, "sim f%u n%d %.0fms frames", m.frame, n, (ag_now() - t0) * 1e3);
        for (int i = 0; i < n; i++) fprintf(stderr, " %d%s", rq[i].frames, rs[i].dead == -1 ? "" : rs[i].dead == -2 ? "x" : "d");
        fprintf(stderr, "\n");
    }
}

static void ag_run(const AgSimReq *rq, AgSimRes *rs, int n)
{
    static int nth;
    if (!nth) { const char *e = getenv("SBM2_SIMTHREADS"); nth = e ? atoi(e) : SIM_THREADS; if (nth < 1 || nth > SIM_THREADS) nth = SIM_THREADS; }
    for (int b = 0; b < n; b += nth) {
        int k = n - b < nth ? n - b : nth;
        pthread_t th[SIM_THREADS];
        for (int i = 0; i < k; i++) {
            const AgSimReq *r = &rq[b + i];
            if (!jobs[i]) jobs[i] = calloc(1, sizeof(SimJob));
            SimJob *j = jobs[i];
            sim_setup(j, r->pre, r->npre, r->frames);
            j->nwp = r->npath;
            j->wp = realloc(j->wp, (size_t)(r->npath ? r->npath : 1) * 4);
            if (r->npath) memcpy(j->wp, r->path, (size_t)r->npath * 4);
            j->npre = r->npath ? r->npre : 0;
            if (r->npath) for (int f = r->npre; f <= r->frames + 2; f++) j->buf[f] = 0;
            free(j->post);
            j->post = malloc((size_t)(r->npost ? r->npost : 1) * 2);
            if (r->npost) memcpy(j->post, r->post, (size_t)r->npost * 2);
            j->npost = r->npost;
            j->wp2 = realloc(j->wp2, (size_t)(r->npath2 ? r->npath2 : 1) * 4);
            if (r->npath2) memcpy(j->wp2, r->path2, (size_t)r->npath2 * 4);
            j->nwp2 = r->npath2;
            j->detEnd = r->detEnd;
            j->detWait = 0;
            j->det = 0;
            j->resume = 0;
            j->done = 0;
            j->trace = rs[b + i].trace;
            j->tcap = rs[b + i].cap;
            j->etrace = r->wantEnemies ? rs[b + i].etrace : NULL;
            j->nboss = r->nboss < 32 ? r->nboss : 32;
            for (int q = 0; q < j->nboss; q++) j->bossBase[q] = r->bossBase[q];
            j->btrace = r->wantBoss && j->nboss ? rs[b + i].btrace : NULL;
        }
        for (int i = 1; i < k; i++) pthread_create(&th[i], NULL, sim_run, jobs[i]);
        sim_run(jobs[0]);
        for (int i = 1; i < k; i++) pthread_join(th[i], NULL);
        sim_runs += k;
        for (int i = 0; i < k; i++) {
            SimJob *j = jobs[i];
            AgSimRes *o = &rs[b + i];
            SimRes r;
            j->trace = NULL;
            j->etrace = NULL;
            j->btrace = NULL;
            sim_result(j, &r);
            o->bossHp = 0;
            for (int q = 0; q < j->nboss; q++) o->bossHp += j->s.wram[j->bossBase[q] + 0x20] | j->s.wram[j->bossBase[q] + 0x21] << 8;
            o->dead = r.dead; o->enemies = r.en; o->hp = r.hp;
            o->enemyMask = agent_enemy_mask(j->s.wram);
            o->px = r.px; o->py = r.py; o->ph = r.ph;
            o->mapSig = agent_map_sig(j->s.wram);
            o->mapChanged = j->mapChanged || o->mapSig != j->map0;
            for (int f = 0; f < o->cap && f <= j->frames + 2; f++) o->pads[f] = j->buf[f];
            ag_simfr += j->frames;
        }
    }
}

/* The agent's pad for this frame (set as the live pad); *text: its new log
 * lines. Also the overlay file's lines (SBM2_OVERLAY). */
static uint16_t agent_call(int fr, const char *goal, const char *extra, const char **text)
{
    if (!agent) agent = sbm2_agent_new(goal);
    if (!agent) { *text = NULL; return 0; }
    double t0 = ag_now();
    uint16_t p = sbm2_agent_frame(agent, m.wram, fr, extra, ag_sim, NULL);
    ag_tagent += ag_now() - t0; ag_calls++;
    live_pad = p;
    const char *lg = sbm2_agent_log(agent);
    *text = lg;
    if (ov_open()) {
        /* a new move: its path and bomb presses */
        static uint64_t lastH;
        const uint16_t *pp;
        const int16_t *tt;
        int at, pn = sbm2_agent_plan(agent, &pp, &tt, &at);
        uint64_t h = 0xCBF29CE484222325ull ^ (uint64_t)pn;
        for (int f = 0; f < 2 * pn; f++) h = (h ^ (uint16_t)tt[f]) * 0x100000001B3ull;
        if (pn && at <= 1 && h != lastH) {
            lastH = h;
            static char lastWhat[64];
            const char *w = sbm2_agent_what(agent);
            if (strncmp(w, lastWhat, sizeof lastWhat - 1)) {
                snprintf(lastWhat, sizeof lastWhat, "%s", w);
                fprintf(ov_f, "W %u %s\n", m.frame, w);
            }
            fprintf(ov_f, "P %u %d %d", m.frame, at, (pn - at + 1) / 2);
            ov_trace(tt, at, pn);
            fputc('\n', ov_f);
            for (int f = at; f < pn; f++)
                if ((pp[f] & PAD_A) && (f == 0 || !(pp[f - 1] & PAD_A))) fprintf(ov_f, "A %u %d %d\n", m.frame, tt[2 * f], tt[2 * f + 1]);
        }
        for (const char *q = lg; *q; ) {
            const char *e = strchr(q, '\n');
            int len = e ? (int)(e - q) : (int)strlen(q);
            if (len) fprintf(ov_f, "L %u %.*s\n", m.frame, len, q);
            q += len + (e ? 1 : 0);
        }
    }
    return p;
}

/* --- the C driver: agentrun without a script ---
 * Plays the agent from SBM2_LOAD (+ SBM2_PREFIX):
 * each frame the agent's pad, with world-view boss lines from exec hooks on
 * the boss code (bot/boss.lua's rules: docs/enemies.md section 7), the log
 * (STAGE / SEED / DEATH / END lines, scenario.py greps them) and the pad
 * log (run.inputs: the start state's sidecar, the prefix, then every pad
 * read and NMI, recorded by snes.c).
 * Env: SBM2_OUT, SBM2_LOAD, SBM2_PREFIX, SBM2_FRAMES, SBM2_FRAME0,
 * SBM2_STOPEARLY (stop at a death and at the next stage), SBM2_STOPAT
 * (hex stage), SBM2_GOAL (default "powerups+exit"), SBM2_SEED,
 * SBM2_AGVERBOSE. */
static struct {
    const char *out, *load, *goal;
    int frames, frame0, stopearly, stopat, seed, verbose;
    int frame, loaded, stage, deadAt, lockAt, deaths, holdUntil;
    char *log; size_t loglen, logcap;
    uint16_t *base; uint32_t nbase; int valid;   /* run.inputs before the recording */
    double t0;
} cd;

/* boss objects the boss code ran on this frame (cur), last and the one before */
enum { CD_OBJ = 64, CD_SENSE = 40 };
typedef struct { int used; uint16_t base; int hasBox, box[4], hurt, ns; int16_t sense[CD_SENSE][2]; } CdObj;
static CdObj cd_obj[3][CD_OBJ];
static int cd_cur;
static uint16_t cd_senseObj;
static int cd_hasSense;

static int cd_r16(uint32_t a) { return m.wram[a] | m.wram[a + 1] << 8; }
static int cd_s16(int v) { return v >= 0x8000 ? v - 0x10000 : v; }
static CdObj *cd_objFor(uint16_t x)
{
    CdObj *t = cd_obj[cd_cur];
    for (int i = 0; i < CD_OBJ; i++) if (t[i].used && t[i].base == x) return &t[i];
    for (int i = 0; i < CD_OBJ; i++) if (!t[i].used) { memset(&t[i], 0, sizeof t[i]); t[i].used = 1; t[i].base = x; return &t[i]; }
    return NULL;
}

/* Exec hooks: BossContactP1 (CC:A538) with the box at [$6D]; the sense
 * checks (BossFlameSense CC:A266, the 4-8 centre check CC:C4B9) note the
 * object, then BossEatBomb (CC:A2C4) runs once per sense point. */
static void cd_exec(uint32_t pc)
{
    if (pc == 0xCCA538) {
        CdObj *o = cd_objFor(m.cpu.x);
        if (!o) return;
        uint32_t p = m.wram[0x6D] | m.wram[0x6E] << 8 | m.wram[0x6F] << 16;
        for (int k = 0; k < 4; k++) {
            /* (straight from ROM: a bus read would touch the machine, open bus) */
            uint32_t a = p + 2 * (uint32_t)k;
            int v = is_rom(a) ? m.rom[rom_offset(a)] | m.rom[rom_offset(a + 1)] << 8 : 0;
            o->box[k] = cd_s16(v) - 0x1000;
        }
        o->hasBox = 1;
    } else if (pc == 0xCCA266 || pc == 0xCCC4B9) {
        cd_senseObj = m.cpu.x;
        cd_hasSense = 1;
    } else if (pc == 0xCCA2C4) {
        uint16_t x = cd_senseObj;
        if (!cd_hasSense || x < 0x1000 || x >= 0x1800) return;
        CdObj *o = cd_objFor(x);
        if (!o) return;
        if (o->ns < CD_SENSE) {
            o->sense[o->ns][0] = (int16_t)(cd_s16(cd_r16(0x4A)) - cd_s16(cd_r16(x + 0x11)));
            o->sense[o->ns][1] = (int16_t)(cd_s16(cd_r16(0x4C)) - cd_s16(cd_r16(x + 0x14)));
            o->ns++;
        }
        o->hurt = cd_r16(x + 0x26) != 0;
    }
}

static void cd_logf(const char *fmt, ...)
{
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof b) n = sizeof b - 1;
    if (cd.loglen + (size_t)n + 2 > cd.logcap) { cd.logcap = (cd.loglen + (size_t)n + 2) * 2; cd.log = realloc(cd.log, cd.logcap); }
    memcpy(cd.log + cd.loglen, b, (size_t)n);
    cd.loglen += (size_t)n;
    cd.log[cd.loglen++] = '\n';
}
static void cd_write(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}
static void cd_flush(void)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/bot_%06d.log", cd.out, cd.frame0);
    cd_write(p, cd.log, cd.loglen);
}
static void cd_finish(int code, const char *why)
{
    cd_logf("%6d END %02X %s deaths %d", cd.frame, m.wram[0x1A], why, cd.deaths);
    cd_logf("%6d STATS wall %.2f calls %d sims %d hits %d simframes %d tsim %f tagent %f", cd.frame, ag_now() - cd.t0,
            (int)ag_calls, (int)ag_nsim, (int)ag_hits, (int)ag_simfr, ag_tsim, ag_tagent);
    cd_flush();
    if (cd.valid) {
        char p[1024];
        snprintf(p, sizeof p, "%s/run.inputs", cd.out);
        FILE *f = fopen(p, "wb");
        if (f) {
            fwrite(cd.base, 2, cd.nbase, f);
            fwrite(m.pad_rec, 2, m.pad_rec_len, f);
            fclose(f);
        }
    }
    stopped = code;
}

/* SBM2_SEED=N (N > 0; scenario.py -s): idle 0..63 frames (picked from N
 * and the stage) at each stage start before the agent plays. The game's
 * RNG runs every frame, so enemies choose differently: other playthroughs
 * of the same stages, to tell luck from skill. 0 = no wait. */
static int cd_seedWait(int st)
{
    if (cd.seed <= 0) return 0;
    uint32_t h = (uint32_t)((uint64_t)cd.seed * 2654435761u + (uint64_t)st * 40503u);
    h = (uint32_t)((uint64_t)(h ^ (h >> 15)) * 2246822519u);
    return (int)((h >> 13) % 64);
}

static int cd_cmpBase(const void *a, const void *b) { return (int)(*(const uint16_t *)a) - (int)(*(const uint16_t *)b); }

/* The world-view boss lines (docs/worldview.md): objects seen last frame
 * (1-8 and 3-8: the frame before too; some run their contact check every
 * other frame), in the x-8 stages only. */
static void cd_bossLines(char *out, size_t cap)
{
    out[0] = 0;
    int stg = m.wram[0x1A];
    if ((stg & 0x0F) != 7) return;
    int last = (cd_cur + 2) % 3, prev = (cd_cur + 1) % 3;
    uint16_t bases[2 * CD_OBJ];
    const CdObj *of[2 * CD_OBJ];
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        int t = pass == 0 ? last : prev;
        if (pass == 1 && stg != 0x17 && stg != 0x37) break;
        for (int i = 0; i < CD_OBJ; i++) {
            const CdObj *o = &cd_obj[t][i];
            if (!o->used) continue;
            int dup = 0;
            for (int k = 0; k < n; k++) dup |= bases[k] == o->base;
            if (dup) continue;
            bases[n] = o->base; of[n++] = o;
        }
    }
    /* by base (Lua's table order was a hash's) */
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && cd_cmpBase(&bases[k - 1], &bases[k]) > 0; k--) {
            uint16_t tb = bases[k]; bases[k] = bases[k - 1]; bases[k - 1] = tb;
            const CdObj *to = of[k]; of[k] = of[k - 1]; of[k - 1] = to;
        }
    int ox = cd_s16(cd_r16(0x5F01)), oy = cd_s16(cd_r16(0x5EFB));
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        const CdObj *o = of[i];
        int b = o->base;
        int x = cd_s16(cd_r16((uint32_t)b + 0x11)), y = cd_s16(cd_r16((uint32_t)b + 0x14));
        if (!(x < 0x100)) x -= 0x100;
        int state = cd_r16((uint32_t)b) | m.wram[b + 2] << 16;
        len += (size_t)snprintf(out + len, cap - len, "%sboss base=%04X state=%06X x=%d y=%d hp=%d hurt=%d", i ? "\n" : "", b, state,
                                x + ox, y + oy, cd_r16((uint32_t)b + 0x20), o->hurt);
        if (o->hasBox && len < cap)
            len += (size_t)snprintf(out + len, cap - len, " box=%d,%d,%d,%d", o->box[0], o->box[1], o->box[2], o->box[3]);
        for (int k = 0; k < o->ns && len < cap; k++)
            len += (size_t)snprintf(out + len, cap - len, "%s%d,%d", k ? ";" : " sense=", o->sense[k][0], o->sense[k][1]);
        if (len >= cap) { out[cap - 1] = 0; return; }
    }
}

static void cd_frame(void)
{
    if (!cd.loaded) {
        /* the start: the run's pad log is the state's sidecar, the prefix, then the recording */
        cd.loaded = 1;
        cd.valid = 1;
        cd.nbase = 0;
        if (cd.load) {
            char side[1024];
            snprintf(side, sizeof side, "%.*s.inputs", (int)strlen(cd.load) - 4, cd.load);
            cd.base = load_words(side, &cd.nbase);
            if (!cd.base) cd.valid = 0;
        }
        const char *prefix = getenv("SBM2_PREFIX");
        uint32_t pn = 0;
        uint16_t *pre = prefix && *prefix ? load_words(prefix, &pn) : NULL;
        if (pre && pn) {
            cd.base = realloc(cd.base, (size_t)(cd.nbase + pn) * 2);
            memcpy(cd.base + cd.nbase, pre, (size_t)pn * 2);
            cd.nbase += pn;
        }
        free(pre);
        /* the recording starts with this frame's NMI, taken just before
         * (snes_step takes it before the hook runs us) */
        if (m.pad_rec_len && (m.pad_rec[m.pad_rec_len - 1] & 0xF)) {
            m.pad_rec[0] = m.pad_rec[m.pad_rec_len - 1];
            m.pad_rec_len = 1;
        } else
            m.pad_rec_len = 0;
        return;
    }
    int st = m.wram[0x1A];
    int inStage = (cd_r16(0x8000) & 0xF000) == 0x9000 && m.wram[0xBC] != 0;
    if (inStage && st != cd.stage) {
        if (st == cd.stopat || (cd.stage >= 0 && cd.stopearly && cd.stopat < 0)) {
            cd_logf("%6d STAGE %02X", cd.frame, st);
            cd_finish(0, "next stage");
            return;
        }
        cd.stage = st;
        cd_logf("%6d STAGE %02X hearts %02X", cd.frame, st, m.wram[0x80EF]);
        int w = cd_seedWait(st);
        if (w > 0) { cd.holdUntil = cd.frame + w; cd_logf("%6d SEED %d wait %d", cd.frame, cd.seed, w); }
    }
    /* A death: P1 locked as C0:A675 leaves it ($52 & $F809 == $0801), not on
     * the exit nor a spring pad, and still locked 90 frames on (the
     * oracle's rule, sim_run). */
    int lock = cd_r16(0x8052);
    int px = (cd_r16(0x8010) + cd_r16(0x5F01)) & 0xFFFF, py = (cd_r16(0x8018) + cd_r16(0x5EFB)) & 0xFFFF;
    int pc = px / 16, pr = py / 16;
    int onPad = pc < 32 && pr < 32 && m.wram[0x1C000 + pr * 0x80 + pc * 2] == 0x4C;
    if ((lock & 0xF809) == 0x0801 && cd_r16(0x80A8) != cd_r16(0x5D72) && !onPad) { if (cd.lockAt < 0) cd.lockAt = cd.frame; }
    else cd.lockAt = -1;
    if (cd.lockAt >= 0 && cd.frame - cd.lockAt >= 90) {
        if (cd.deadAt < 0) {
            cd.deadAt = cd.frame;
            cd.deaths++;
            cd_logf("%6d DEATH at px(%d,%d) (locked since %d)", cd.frame, cd_r16(0x8010), cd_r16(0x8018), cd.lockAt);
            char p[1024];
            snprintf(p, sizeof p, "%s/wv_death_%06d.txt", cd.out, cd.frame);
            const char *wv = agent ? sbm2_agent_world(agent) : "";
            cd_write(p, wv, strlen(wv));
            if (cd.stopearly) { cd_finish(1, "death"); return; }
        }
    } else
        cd.deadAt = -1;
    /* the boss objects: this frame's list becomes the last */
    cd_cur = (cd_cur + 1) % 3;
    memset(cd_obj[cd_cur], 0, sizeof cd_obj[cd_cur]);
    if (cd.frame < cd.holdUntil) {
        live_pad = 0;
        if (++cd.frame >= cd.frames) cd_finish(0, "budget");
        return;
    }
    static char bl[16384];
    cd_bossLines(bl, sizeof bl);
    const char *text;
    agent_call(cd.frame, cd.goal, bl, &text);
    if (!text) { cd_logf("%6d agent error: bad goal", cd.frame); cd_finish(4, "error"); return; }
    if (*text) {
        for (const char *q = text; *q; ) {
            const char *e = strchr(q, '\n');
            int len = e ? (int)(e - q) : (int)strlen(q);
            if (len) cd_logf("%.*s", len, q);
            q += len + (e ? 1 : 0);
        }
        if (cd.verbose) {
            char p[1024];
            snprintf(p, sizeof p, "%s/wv_%06d.txt", cd.out, cd.frame);
            const char *wv = sbm2_agent_world(agent);
            cd_write(p, wv, strlen(wv));
        }
    }
    if (cd.frame % 600 == 0) cd_flush();
    if (++cd.frame >= cd.frames) cd_finish(0, "budget");
}

static void cd_init(void)
{
    const char *e;
    cdrv = 1;
    cd.out = (e = getenv("SBM2_OUT")) ? e : "build/agent";
    cd.frames = (e = getenv("SBM2_FRAMES")) ? atoi(e) : 3600;
    cd.frame0 = (e = getenv("SBM2_FRAME0")) ? atoi(e) : 0;
    cd.load = (e = getenv("SBM2_LOAD")) && *e ? e : NULL;
    cd.goal = (e = getenv("SBM2_GOAL")) ? e : "powerups+exit";
    cd.stopearly = (e = getenv("SBM2_STOPEARLY")) && !strcmp(e, "1");
    cd.verbose = (e = getenv("SBM2_AGVERBOSE")) && *e && strcmp(e, "0");
    cd.stopat = (e = getenv("SBM2_STOPAT")) && *e ? (int)strtol(e, NULL, 16) : -1;
    cd.seed = (e = getenv("SBM2_SEED")) ? atoi(e) : 0;
    cd.frame = cd.frame0;
    cd.loaded = cd.load == NULL;
    cd.stage = cd.deadAt = cd.lockAt = -1;
    cd.t0 = ag_now();
    m.pad_rec_len = 0;
    m.pad_record = 1;
    if (cd.loaded) cd.valid = 1;
}

#endif

static int l_padBits(lua_State *Ls)
{
    static const struct { const char *k; uint16_t bit; } map[] = {
        {"a", PAD_A}, {"b", PAD_B}, {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT}};
    luaL_checktype(Ls, 1, LUA_TTABLE);
    uint16_t p = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        lua_getfield(Ls, 1, map[i].k);
        if (lua_toboolean(Ls, -1)) p |= map[i].bit;
        lua_pop(Ls, 1);
    }
    lua_pushinteger(Ls, p);
    return 1;
}

static int l_setInput(lua_State *Ls)
{
    static const struct { const char *k; uint16_t bit; } map[] = {
        {"a", PAD_A}, {"b", PAD_B}, {"x", PAD_X}, {"y", PAD_Y}, {"l", PAD_L}, {"r", PAD_R},
        {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
        {"start", PAD_START}, {"select", PAD_SELECT}};
    luaL_checktype(Ls, 1, LUA_TTABLE);
    uint16_t p = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        lua_getfield(Ls, 1, map[i].k);
        if (lua_toboolean(Ls, -1)) p |= map[i].bit;
        lua_pop(Ls, 1);
    }
    live_pad = p;
    return 0;
}

static int l_getState(lua_State *Ls)
{
    lua_createtable(Ls, 0, 10);
#define F(k, v) lua_pushinteger(Ls, v); lua_setfield(Ls, -2, k);
    F("cpu.a", m.cpu.a) F("cpu.x", m.cpu.x) F("cpu.y", m.cpu.y) F("cpu.sp", m.cpu.sp)
    F("cpu.d", m.cpu.d) F("cpu.pc", m.cpu.pc) F("cpu.k", m.cpu.pb) F("cpu.dbr", m.cpu.db)
    F("cpu.ps", m.cpu.p) F("frameCount", m.frame)
#undef F
    return 1;
}

static int l_createSavestate(lua_State *Ls) { push_state(Ls); return 1; }

static int l_loadSavestate(lua_State *Ls)
{
    size_t n;
    const char *d = luaL_checklstring(Ls, 1, &n);
    live_gen++;
    if (n >= 8 && !memcmp(d, MAGIC, 8)) {
        restore_state(d, n);
    } else if (!start_state) {
        /* the Mesen start state: we are at it; remember this point */
        push_state(Ls);
        const char *s = lua_tolstring(Ls, -1, &start_len);
        start_state = malloc(start_len);
        memcpy(start_state, s, start_len);
        lua_pop(Ls, 1);
        return 0;
    } else {
        restore_state(start_state, start_len);
    }
    loaded_state = 1;
    return 0;
}

static int l_stop(lua_State *Ls) { stopped = luaL_optinteger(Ls, 1, 0); return 0; }
static int l_takeScreenshot(lua_State *Ls) { lua_pushliteral(Ls, ""); return 1; }

static int l_getCdlString(lua_State *Ls)
{
    lua_pushlstring(Ls, (const char *)cdl, m.rom_size);
    return 1;
}

static int l_getCdlData(lua_State *Ls)
{
    lua_createtable(Ls, m.rom_size, 0);
    for (uint32_t i = 0; i < m.rom_size; i++) { lua_pushinteger(Ls, cdl[i]); lua_rawseti(Ls, -2, i); }
    return 1;
}

#include <time.h>
/* emu.now(): a monotonic wall clock in seconds (profiling) */
static int l_now(lua_State *Ls)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushnumber(Ls, ts.tv_sec + ts.tv_nsec * 1e-9);
    return 1;
}
static int l_simStats(lua_State *Ls)
{
    lua_pushinteger(Ls, (lua_Integer)sim_runs);
    lua_pushinteger(Ls, (lua_Integer)sim_hits0);
    lua_pushinteger(Ls, (lua_Integer)sim_hitsD);
    return 3;
}
static int l_log(lua_State *Ls) { fprintf(stderr, "%s\n", luaL_checkstring(Ls, 1)); return 0; }

static void set_enum(const char *name, const char *const *keys, int n)
{
    lua_createtable(L, 0, n);
    for (int i = 0; i < n; i++) { lua_pushinteger(L, i); lua_setfield(L, -2, keys[i]); }
    lua_setfield(L, -2, name);
}

static void open_emu(void)
{
    static const luaL_Reg fns[] = {
        {"read", l_read}, {"read16", l_read16}, {"readBytes", l_readBytes}, {"write", l_write}, {"write16", l_write16},
        {"getMemorySize", l_getMemorySize}, {"addMemoryCallback", l_addMemoryCallback},
        {"removeMemoryCallback", l_removeMemoryCallback}, {"addEventCallback", l_addEventCallback},
        {"setInput", l_setInput}, {"getState", l_getState}, {"createSavestate", l_createSavestate},
        {"loadSavestate", l_loadSavestate}, {"stop", l_stop}, {"takeScreenshot", l_takeScreenshot},
        {"simulate", l_simulate}, {"simulateMany", l_simulateMany}, {"simStats", l_simStats}, {"now", l_now}, {"simHearts", l_simHearts}, {"padBits", l_padBits},
        {"getCdlData", l_getCdlData}, {"getCdlString", l_getCdlString}, {"log", l_log},
        {NULL, NULL}};
    luaL_newlib(L, fns);
    static const char *const mt[] = {"snesMemory", "snesWorkRam", "snesPrgRom"};
    set_enum("memType", mt, 3);
    static const char *const ct[] = {"read", "write", "exec"};
    set_enum("callbackType", ct, 3);
    static const char *const et[] = {"endFrame", "inputPolled"};
    set_enum("eventType", et, 2);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "isPort");
    lua_setglobal(L, "emu");
}

/* --- start state --- */
static uint16_t *load_words(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(size + 1);
    uint16_t *w = calloc(size / 2 + 1, 2);
    if (fread(b, 1, size, f) != (size_t)size) size = 0;
    fclose(f);
    for (long i = 0; i + 1 < size; i += 2) w[i / 2] = b[i] | b[i + 1] << 8;
    free(b);
    *len = size / 2;
    return w;
}

static char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(size + 1);
    *n = fread(d, 1, size, f);
    fclose(f);
    return d;
}

/* Replays a pad log in frame-step mode, from the current state, up to the
 * end of the logic frame where it runs out (where Mesen's endFrame saved a
 * state with this log as its sidecar), then leaves pad-log mode. */
static int replay_words(uint16_t *log, uint32_t n, const char *what)
{
    m.pads = log;
    m.npads = n;
    m.pad_pos = 0;
    m.pad_desync = 0;
    m.pad_markers = 0;
    for (uint32_t i = 0; i < n; i++) if (log[i] & 0xF) { m.pad_markers = 1; break; }
    for (long steps = 0;; steps++) {
        uint32_t pc = (uint32_t)m.cpu.pb << 16 | m.cpu.pc;
        if (m.pad_pos >= m.npads && !m.fs_raised && !m.fs_in_nmi && !m.cpu.nmi &&
            m.wait_at[pc & 0xFFFF] && snes_wait_point(&m, pc) == WAIT_CMP)
            break;
        if (steps > 4000000000L) { fprintf(stderr, "botrun: replay of %s did not end\n", what); return -1; }
        snes_step(&m);
    }
    if (m.pad_desync) fprintf(stderr, "botrun: %u pad-log entries out of place in %s\n", m.pad_desync, what);
    m.pads = NULL;
    m.npads = m.pad_pos = 0;
    m.pad_markers = 0;
    return 0;
}

/* SBM2_WRAMAT=<frame>:<file>: write WRAM to file at that video frame's
 * first wait loop (the game idle, waiting for the NMI): the same point in
 * any run of the same machine, to check a replay or a scenario's prefix
 * against the run it came from. */
static void wram_at(void)
{
    static long at = -2;
    static const char *path;
    if (at == -2) {
        const char *e = getenv("SBM2_WRAMAT");
        at = -1;
        if (e && *e && strchr(e, ':')) { at = atol(e); path = strchr(e, ':') + 1; }
    }
    if (at < 0 || m.frame != (uint32_t)at) return;
    uint32_t pc = (uint32_t)m.cpu.pb << 16 | m.cpu.pc;
    if (!m.wait_at[pc & 0xFFFF] || snes_wait_point(&m, pc) != WAIT_CMP) return;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(m.wram, 1, 0x20000, f); fclose(f); }
    at = -1;
}

/* Replays a pad log recorded in timed mode (a cut of a botrun/agentrun
 * run.inputs) in timed mode, from the current state, as SBM2_REPLAY does:
 * each pad read takes the next value, the NMI markers are counted, not
 * placed. It stops at the first vblank after the last read, where the
 * recorded run's vblank callbacks chose the next pad: the bot takes over
 * there. (In
 * frame-step the markers had to be placed, which over a long cut went
 * wrong: "pad-log entries out of place", a different game from there.) */
static int replay_timed_vb;
static void replay_timed_mark(Snes *s) { (void)s; replay_timed_vb = 1; }
static int replay_timed(uint16_t *log, uint32_t n, const char *what)
{
    uint32_t marks = 0, taken = 0, reads = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!(log[i] & 0xF)) reads++;
        else if ((log[i] & 0xF) == PADLOG_POKE) { fprintf(stderr, "botrun: %s has pokes: not a timed log\n", what); return -1; }
        else marks++;
    }
    void (*keep)(Snes *) = m.on_vblank;
    int keepfs = m.framestep;
    m.framestep = 0;
    m.on_vblank = replay_timed_mark;
    m.pads = log;
    m.npads = n;
    m.pad_pos = 0;
    m.pad_desync = 0;
    m.pad_markers = 0;
    for (long steps = 0;; steps++) {
        int pend = m.nmi_pending;
        replay_timed_vb = 0;
        snes_step(&m);
        wram_at();
        if (pend && !m.nmi_pending) taken++;
        if (replay_timed_vb && m.pad_pos - m.pad_desync >= reads && (m.nmitimen & 0x80)) break;
        if (steps > 4000000000L) { fprintf(stderr, "botrun: replay of %s did not end\n", what); return -1; }
    }
    /* every read used, and every NMI where the log has one */
    if (m.pad_pos - m.pad_desync != reads || taken != marks)
        fprintf(stderr, "botrun: %s: %u of %u pad reads used, %u NMIs for %u markers\n", what,
                m.pad_pos - m.pad_desync, reads, taken, marks);
    m.pads = NULL;
    m.npads = m.pad_pos = 0;
    m.on_vblank = keep;
    m.framestep = keepfs;
    vb_pending = 1;   /* that vblank's callbacks: the bot's first frame */
    return 0;
}

/* The pad log of a Mesen state, from power-on. */
static int replay_to(const char *state)
{
    char side[1024];
    snprintf(side, sizeof side, "%.*s.inputs", (int)strlen(state) - 4, state);
    uint32_t n;
    uint16_t *log = load_words(side, &n);
    if (!log) { fprintf(stderr, "botrun: no pad log %s for %s\n", side, state); return -1; }
    int rc = replay_words(log, n, side);
    free(log);
    return rc;
}

/* The replayed Mesen state, cached as "<state>.port" (a port state) while
 * it is newer than the pad log. */
static void cache_path(char *buf, size_t n, const char *state)
{
    snprintf(buf, n, "%.*s.port", (int)strlen(state) - 4, state);
}

static int load_cached(const char *state)
{
    char p[1024], side[1024];
    cache_path(p, sizeof p, state);
    snprintf(side, sizeof side, "%.*s.inputs", (int)strlen(state) - 4, state);
    struct stat a, b;
    if (stat(p, &a) || stat(side, &b) || a.st_mtime < b.st_mtime) return 0;
    size_t n;
    char *d = read_file(p, &n);
    if (!d) return 0;
    int ok = restore_state(d, n);
    free(d);
    skip_end_frame = 0;
    return ok;
}

static void save_cached(const char *state)
{
    char p[1024], tmp[1100];
    cache_path(p, sizeof p, state);
    snprintf(tmp, sizeof tmp, "%s.%d", p, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint8_t flag = 0;
    fwrite(MAGIC, 1, 8, f);
    fwrite(&flag, 1, 1, f);
    fwrite(&m, 1, sizeof m, f);
    if (m.sram_size) fwrite(m.sram, 1, m.sram_size, f);
    fclose(f);
    rename(tmp, p);
}

int main(int argc, char **argv)
{
#ifdef SBM2_AGENT
    /* agentrun <rom>: the C driver plays the agent (no script) */
    int noscript = argc == 2;
#else
    int noscript = 0;
#endif
    if (argc < 3 && !noscript) { fprintf(stderr, "usage: %s <rom> <script.lua>\n", argv[0]); return 2; }
    if (snes_load(&m, argv[1])) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
    snes_power(&m);
    m.framestep = 1;
    input = calloc(INPUT_LEN, 2);
    m.input = input;
    m.input_len = INPUT_LEN;
    cdl = calloc(m.rom_size, 1);
    cb_bounds();

    /* SBM2_REPLAY=<run.inputs>: play a pad log recorded in timed mode back
     * in timed mode from power-on (each pad read takes the next value, the
     * NMI markers are skipped: the same machine replays the same run), then
     * print the stage ($1A) and frame count and exit. Verifies a whole run. */
    const char *rep = getenv("SBM2_REPLAY");
    if (rep && *rep) {
        uint32_t n;
        uint16_t *log = load_words(rep, &n);
        if (!log) { fprintf(stderr, "botrun: can't read %s\n", rep); return 2; }
        /* SBM2_REPLAY_START=<Mesen state>: the run began there (botrun got
         * to it in frame-step, then went timed): replay its sidecar the same
         * way first, then the rest of the log timed. */
        const char *rs = getenv("SBM2_REPLAY_START");
        uint32_t skip = 0;
        if (rs && *rs) {
            char side[1024];
            snprintf(side, sizeof side, "%.*s.inputs", (int)strlen(rs) - 4, rs);
            uint32_t sn;
            uint16_t *sl = load_words(side, &sn);
            if (!sl || sn > n || memcmp(sl, log, sn * 2)) { fprintf(stderr, "botrun: %s is not a prefix of the log\n", side); return 2; }
            /* A port state (a run saved by botrun) is restored as is: its
             * sidecar may itself be stitched from timed runs and not replay
             * from power-on in frame-step. */
            size_t pn;
            char *pd = read_file(rs, &pn);
            if (pd && pn >= 8 && !memcmp(pd, MAGIC, 8)) {
                if (!restore_state(pd, pn)) { fprintf(stderr, "botrun: %s is a port state of another build\n", rs); return 2; }
                printf("start: port state %s, frame %u stage %02X\n", rs, m.frame, m.wram[0x1A]);
            } else if (replay_words(sl, sn, side)) return 2;
            free(pd);
            skip = sn;
        }
        m.framestep = 0;
        m.pads = log + skip;
        m.npads = n - skip;
        m.pad_pos = 0;
        uint8_t last = 0xFF;
        while (m.pad_pos < m.npads) {
            snes_step(&m);
            wram_at();
            if (m.wram[0x1A] != last && m.wram[0xBC]) {
                last = m.wram[0x1A];
                printf("frame %u stage %02X (pad log %u/%u)\n", m.frame, last, m.pad_pos, m.npads);
            }
        }
        printf("end: frame %u stage %02X, %u log entries skipped\n", m.frame, m.wram[0x1A], m.pad_desync);
        /* SBM2_REPLAY_CMP=<port state>: compare WRAM with that state's */
        const char *cmp = getenv("SBM2_REPLAY_CMP");
        if (cmp && *cmp) {
            size_t sn;
            char *d = read_file(cmp, &sn);
            if (d && sn >= 9 + sizeof m && !memcmp(d, MAGIC, 8)) {
                const Snes *o = (const Snes *)(d + 9);
                int diff = 0, first = -1;
                for (int i = 0; i < 0x20000; i++) if (o->wram[i] != m.wram[i]) { if (first < 0) first = i; diff++; }
                printf("vs %s: %d WRAM bytes differ (first %05X), state frame %u\n", cmp, diff, first, o->frame);
            }
        }
        return 0;
    }
    const char *load = getenv("SBM2_LOAD");
    if (load && *load) {
        size_t n;
        char *d = read_file(load, &n);
        if (!d) { fprintf(stderr, "botrun: can't read %s\n", load); return 2; }
        if (n >= 8 && !memcmp(d, MAGIC, 8)) {
            if (!restore_state(d, n)) {
                fprintf(stderr, "botrun: %s is a port state of another build (size %zu, want %zu)\n",
                        load, n, 9 + sizeof m + m.sram_size);
                return 2;
            }
            skip_end_frame = 0;
        } else if (load_cached(load)) {
            /* replayed before */
        } else {
            if (replay_to(load)) return 2;
            save_cached(load);
        }
        free(d);
    }
    /* SBM2_PREFIX: a pad log to play from there before the bot takes over
     * (bot/scenario.py: a moment of an earlier run, from its stage start). */
    const char *prefix = getenv("SBM2_PREFIX");
    if (prefix && *prefix) {
        uint32_t n;
        uint16_t *log = load_words(prefix, &n);
        /* SBM2_PREFIX_TIMED=1 (scenario.py make writes it): the prefix is a
         * cut of a timed run on this machine (agentrun/botrun run.inputs):
         * replay it timed, exactly. Older scenarios' prefixes (Mesen, older
         * builds) replay in frame-step as they always have. */
        const char *tm0 = getenv("SBM2_TIMED"), *pt = getenv("SBM2_PREFIX_TIMED");
        int fs = (tm0 && !strcmp(tm0, "0")) || !(pt && *pt == '1');
        if (!log || (fs ? replay_words(log, n, prefix) : replay_timed(log, n, prefix))) {
            fprintf(stderr, "botrun: bad prefix %s\n", prefix);
            return 2;
        }
        free(log);
    }
    /* Frame-step frames index input[] by m.frame; keep it within the buffer. */
    bus_read = m.cpu.read;
    bus_write = m.cpu.write;
    cb_bounds();
    m.hook = step_hook;
#ifdef SBM2_AGENT
    cdl_on = getenv("SBM2_CDL") && !strcmp(getenv("SBM2_CDL"), "1");
#endif
    /* SBM2_TIMED=0: frame-step (one video frame per logic frame, no lag).
     * Default: the timed machine, which has Mesen's lag frames. */
    const char *tm = getenv("SBM2_TIMED");
    if (tm && !strcmp(tm, "0")) {
        m.framestep = 1;
        m.on_frame = frame_cb;
    } else {
        m.framestep = 0;
        m.on_vblank = vblank_cb;
    }

#ifdef SBM2_AGENT
    if (noscript) {
        cd_init();
        while (stopped < 0) {
            snes_step(&m);
            wram_at();
            if (m.cpu.stp) { fprintf(stderr, "agentrun: CPU stopped\n"); return 3; }
        }
        return stopped;
    }
#endif
    SANDBOX_ALLOW_LOADFILE = 1;
    L = luaL_newstate();
    luaL_openlibs(L);
    open_emu();
    lua_createtable(L, 1, 0);
    lua_pushstring(L, argv[2]);
    lua_rawseti(L, -2, 0);
    lua_setglobal(L, "arg");
    if (luaL_loadfile(L, argv[2]) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        return 1;
    }
    while (stopped < 0) {
        snes_step(&m);
        wram_at();
        if (m.cpu.stp) { fprintf(stderr, "botrun: CPU stopped\n"); return 3; }
    }
    lua_close(L);
    return stopped;
}
