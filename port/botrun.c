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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "snes.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

extern int SANDBOX_ALLOW_LOADFILE;  /* Mesen's sandbox switch (lauxlib.c) */

static Snes m;
static lua_State *L;
static uint8_t *cdl;
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

static void cb_bounds(void)
{
    for (int t = 0; t < 3; t++) { cb_lo[t] = 0xFFFFFFFF; cb_hi[t] = 0; }
    for (int i = 0; i < ncb; i++) {
        if (!cbs[i].live) continue;
        int t = cbs[i].type;
        if (cbs[i].lo < cb_lo[t]) cb_lo[t] = cbs[i].lo;
        if (cbs[i].hi > cb_hi[t]) cb_hi[t] = cbs[i].hi;
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

static int step_hook(Snes *s)
{
    uint32_t pc = (uint32_t)s->cpu.pb << 16 | s->cpu.pc;
    if (is_rom(pc)) {
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
static void vblank_cb(Snes *s)
{
    (void)s;
    for (int i = 0; i < nev[EV_END_FRAME]; i++) call_ref(ev_ref[EV_END_FRAME][i], 0, 0, 0);
    for (int i = 0; i < nev[EV_INPUT_POLLED]; i++) call_ref(ev_ref[EV_INPUT_POLLED][i], 0, 0, 0);
    input[m.frame % INPUT_LEN] = live_pad;
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

static int l_write(lua_State *Ls)
{
    poke(luaL_optinteger(Ls, 3, MT_CPU), luaL_checkinteger(Ls, 1), luaL_checkinteger(Ls, 2));
    return 0;
}

static int l_write16(lua_State *Ls)
{
    uint32_t a = luaL_checkinteger(Ls, 1);
    int v = luaL_checkinteger(Ls, 2), t = luaL_optinteger(Ls, 3, MT_CPU);
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
        {"getCdlData", l_getCdlData}, {"getCdlString", l_getCdlString}, {"log", l_log}, {NULL, NULL}};
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
    if (argc < 3) { fprintf(stderr, "usage: %s <rom> <script.lua>\n", argv[0]); return 2; }
    if (snes_load(&m, argv[1])) { fprintf(stderr, "can't load %s\n", argv[1]); return 2; }
    snes_power(&m);
    m.framestep = 1;
    input = calloc(INPUT_LEN, 2);
    m.input = input;
    m.input_len = INPUT_LEN;
    cdl = calloc(m.rom_size, 1);
    cb_bounds();

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
        if (!log || replay_words(log, n, prefix)) { fprintf(stderr, "botrun: bad prefix %s\n", prefix); return 2; }
        free(log);
    }
    /* Frame-step frames index input[] by m.frame; keep it within the buffer. */
    bus_read = m.cpu.read;
    bus_write = m.cpu.write;
    m.cpu.read = cb_read;
    m.cpu.write = cb_write;
    m.hook = step_hook;
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
        if (m.cpu.stp) { fprintf(stderr, "botrun: CPU stopped\n"); return 3; }
    }
    lua_close(L);
    return stopped;
}
