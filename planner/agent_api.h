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

#define AG_BLASTS 8
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
    int enemyMask;     /* agent_enemy_mask at the end */
    int16_t *btrace;   /* 2 * nboss * cap (wantBoss, else NULL): each boss's world x, y per frame */
    int cap;
    int16_t blasts[AG_BLASTS]; /* cells (c | r << 8) where P1's bombs went off in the run, in order (agent_p1_blasts) */
    int nblast;
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

/* The move being played (for overlays): its pads and P1's world x, y at the
 * start of each of its frames (2 per frame); *at = the frame playing now.
 * Returns the move's length in frames (0: none). Valid until the next
 * sbm2_agent_frame. */
int sbm2_agent_plan(void *agent, const uint16_t **pads, const int16_t **trace, int *at);
/* What the move is for ("survive", "do", ...), the last decision's kind. */
const char *sbm2_agent_what(void *agent);

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

/* Live enemies and their HP as botrun's sim_result counts them. A blast
 * turns a $90 or $98 into a harmless mount (+$BE bit 15, bit 14 clear) and
 * spawns a $99 rider: the mount is no enemy. Otherwise they count (3-1: a
 * $98 never counted, its hits looked like misses). */
static inline int agent_is_enemy(const uint8_t *w, int base)
{
    int hi = w[base + 1] & 0xF0, ty = w[base + 0x7E];
    return (hi == 0x90 || hi == 0xA0) && !(w[base + 5] & 0x80) && ty < 0xC0 &&
           !((ty == 0x90 || ty == 0x98) && (w[base + 0xBF] & 0xC0) == 0x80);
}
static inline void agent_enemies(const uint8_t *w, int *en, int *hp)
{
    *en = *hp = 0;
    for (int base = 0x8400; base <= 0x8F00; base += 0x100)
        if (agent_is_enemy(w, base)) {
            (*en)++;
            *hp += w[base + 0x43] & 0x7F;
        }
}
/* Which slots hold an enemy (that rule): bit i = $8400 + $100 * i. A kill
 * shows as its slot's bit gone even when another enemy appears (a $98 hit:
 * a mount, and a new $99 rider). */
static inline int agent_enemy_mask(const uint8_t *w)
{
    int m = 0;
    for (int i = 0; i < 12; i++) m |= agent_is_enemy(w, 0x8400 + 0x100 * i) << i;
    return m;
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

/* P1's bombs that start going off (bomb table $12000, 40 slots of $100:
 * +1 bit 7 in use, bit 6 going off, bit 3 an enemy's; +2 the TileMap
 * offset, as planner/capture.cpp reads them): each one's cell, once, into
 * out (c | r << 8, up to AG_BLASTS). *seen: the slots already counted (a
 * slot freed is cleared). Kicked and thrown bombs end up where they end up:
 * this is where they went off. */
static inline void agent_p1_blasts(const uint8_t *w, uint64_t *seen, int16_t *out, int *n)
{
    for (int i = 0; i < 40; i++) {
        int base = 0x12000 + 0x100 * i, fl = w[base + 1];
        uint64_t bit = (uint64_t)1 << i;
        if (!(fl & 0x80)) { *seen &= ~bit; continue; }
        if ((fl & 0x08) || !(fl & 0x40) || (*seen & bit)) continue;
        *seen |= bit;
        int cell = w[base + 2] | w[base + 3] << 8;
        if (*n < AG_BLASTS) out[(*n)++] = (int16_t)(((cell & 0x3F) / 2) | (cell >> 6) << 8);
    }
}

/* P1's cell, packed as agent_enemy_cells packs an enemy's. */
static inline int16_t agent_p1_cell(const uint8_t *w)
{
    int x = (int16_t)(w[0x8010] | w[0x8011] << 8) + (int16_t)(w[0x5F01] | w[0x5F02] << 8);
    int y = (int16_t)(w[0x8018] | w[0x8019] << 8) + (int16_t)(w[0x5EFB] | w[0x5EFC] << 8);
    return x >= 0 && y >= 0 && x < 16 * 64 && y < 16 * 64 ? (int16_t)((x >> 4) | (y >> 4) << 8) : -1;
}

/* The cells of the enemies that hurt on contact: agent_enemy_cells less the
 * types C5:D850 lists as harmless ($16 $24-$28 $30 $31, type & $7F). */
static inline void agent_contact_cells(const uint8_t *w, int16_t *out)
{
    agent_enemy_cells(w, out);
    for (int i = 0; i < AG_ESLOTS; i++) {
        int t = w[0x8400 + 0x100 * i + 0x7E] & 0x7F;
        if (t == 0x16 || (t >= 0x24 && t <= 0x28) || t == 0x30 || t == 0x31) out[i] = -1;
    }
}

/* A pass-through: from one frame to the next, P1 and a harmful enemy swap
 * cells. The game only kills on the same cell (docs/enemies.md 3), and a
 * cell flips half-way between centres, so two walking head-on can cross
 * the line on the same frame and walk through each other (4-2, frame
 * 119361: P1 down the dead end through a $93). The sims count it as a
 * death: the agent must not walk through enemies. Returns the slot + 1. */
static inline int agent_passed_through(int16_t p0, int16_t p1, const int16_t *e0, const int16_t *e1)
{
    if (p0 < 0 || p1 < 0 || p0 == p1) return 0;
    for (int i = 0; i < AG_ESLOTS; i++)
        if (e0[i] == p1 && e1[i] == p0) return i + 1;
    return 0;
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
