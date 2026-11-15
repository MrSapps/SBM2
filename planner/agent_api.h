/* C glue for the C++ agent (planner/agent.hpp), for port/botrun.c.
 *
 * The host owns the emulator. Each logic frame it hands the agent WRAM and a
 * run-ahead oracle; the agent returns the pad. The oracle runs copies of the
 * live machine: a pad prefix, then a closed-loop walk along pixel waypoints,
 * then `post` pads (a bomb press), then a second walk, then nothing (or B for
 * a remote bomb). It reports the pads it pressed frame by frame, so the
 * agent can play exactly those live: the game is deterministic, so a run
 * that lived in the oracle lives live. */
#ifndef SBM2_AGENT_API_H
#define SBM2_AGENT_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint16_t *pre; int npre;      /* pads, one per frame */
    const int16_t *path; int npath;     /* waypoints: x, y world pixels */
    const uint16_t *post; int npost;
    const int16_t *path2; int npath2;
    int detEnd;                          /* press B this many times (20 frames apart) once path2 is done */
    int frames;
    int wantEnemies;                     /* fill AgSimRes.etrace */
    const uint16_t *bossBase; int nboss; /* boss objects: their HP summed at the end */
    int wantBoss;                        /* fill AgSimRes.btrace */
} AgSimReq;

typedef struct {
    int dead;          /* frame offset of the death, -1 lived, -2 left by the exit */
    int enemies, hp;   /* live enemies (and their HP) at the end, as botrun counts them */
    int px, py, ph;    /* P1 at the end (world pixels), controller phase */
    uint64_t mapSig;   /* TileMap hash at the end (agent_map_sig) */
    int mapChanged;    /* the TileMap differed from the start at some point (or at the end) */
    uint16_t *pads;    /* cap entries: the pad of each frame (host fills) */
    int16_t *trace;    /* 2 * cap: P1 world x, y at the start of each frame */
    int16_t *etrace;   /* AG_ESLOTS * cap (wantEnemies, else NULL): agent_enemy_cells per frame */
    int bossHp;        /* sum of +$20 over bossBase at the end */
    int16_t *btrace;   /* 2 * nboss * cap (wantBoss, else NULL): each boss's world x, y per frame */
    int cap;
} AgSimRes;

typedef void (*AgSimFn)(void *ctx, const AgSimReq *reqs, AgSimRes *res, int n);

void *sbm2_agent_new(const char *goal);
void sbm2_agent_free(void *agent);
/* The pad for this frame. extra: world-view lines the host adds (boss lines
 * from its exec hooks, docs/worldview.md), or NULL. */
uint16_t sbm2_agent_frame(void *agent, const uint8_t *wram, int frame, const char *extra, AgSimFn sim, void *ctx);
/* Log lines since the last call ("" if none); valid until the next call. */
const char *sbm2_agent_log(void *agent);
/* The world view of the last decision (docs/worldview.md), for repros. */
const char *sbm2_agent_world(void *agent);

/* The world view of this WRAM as snapshot text (planner/capture.hpp);
 * valid until the next call. */
const char *sbm2_capture(const uint8_t *wram, int frame);

/* The hash the oracle reports as mapSig: FNV-1a over the 32x32 TileMap
 * ($7FC000, one byte per cell). */
static inline uint64_t agent_map_sig(const uint8_t *wram)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 32; c++) h = (h ^ wram[0x1C000 + r * 0x80 + c * 2]) * 0x100000001B3ull;
    return h;
}

/* Live enemies and their HP as botrun's sim_result counts them. */
static inline void agent_enemies(const uint8_t *w, int *en, int *hp)
{
    *en = *hp = 0;
    for (int base = 0x8400; base <= 0x8F00; base += 0x100) {
        int hi = w[base + 1] & 0xF0, ty = w[base + 0x7E];
        if ((hi == 0x90 || hi == 0xA0) && !(w[base + 5] & 0x80) && ty < 0xC0 && ty != 0x90 && ty != 0x98) {
            (*en)++;
            *hp += w[base + 0x43] & 0x7F;
        }
    }
}

/* The enemy slots ($8400..$8F00) and each one's cell, as the world view
 * places it ((x + origin) / 16, packed c | r << 8), or -1 when the slot holds
 * no live enemy as agent_enemies counts them. */
#define AG_ESLOTS 12
static inline void agent_enemy_cells(const uint8_t *w, int16_t *out)
{
    int ox = (int16_t)(w[0x5F01] | w[0x5F02] << 8), oy = (int16_t)(w[0x5EFB] | w[0x5EFC] << 8);
    for (int i = 0; i < AG_ESLOTS; i++) {
        int base = 0x8400 + 0x100 * i, hi = w[base + 1] & 0xF0, ty = w[base + 0x7E];
        out[i] = -1;
        if ((hi == 0x90 || hi == 0xA0) && !(w[base + 5] & 0x80) && ty < 0xC0 && ty != 0x90 && ty != 0x98) {
            int x = (int16_t)(w[base + 0x10] | w[base + 0x11] << 8) + ox, y = (int16_t)(w[base + 0x18] | w[base + 0x19] << 8) + oy;
            if (x >= 0 && y >= 0 && x < 16 * 64 && y < 16 * 64) out[i] = (int16_t)((x >> 4) | (y >> 4) << 8);
        }
    }
}

/* A boss object's world position (+$11/+$14 + origin; a blink's +$100 off). */
static inline void agent_boss_xy(const uint8_t *w, int base, int16_t *x, int16_t *y)
{
    int ox = (int16_t)(w[0x5F01] | w[0x5F02] << 8), oy = (int16_t)(w[0x5EFB] | w[0x5EFC] << 8);
    int bx = (int16_t)(w[base + 0x11] | w[base + 0x12] << 8), by = (int16_t)(w[base + 0x14] | w[base + 0x15] << 8);
    if (bx >= 0x100) bx -= 0x100;
    *x = (int16_t)(bx + ox); *y = (int16_t)(by + oy);
}

#ifdef __cplusplus
}
#endif
#endif
