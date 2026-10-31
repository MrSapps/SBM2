/* Super Bomberman 2 routines ported to C. Each replaces the ROM routine at
 * its address (see hook.h); names follow tools/sbm2/symbols.txt.
 *
 * The routines keep the original's register and flag outputs, since callers
 * written in asm may read them; where that takes bookkeeping (carry and
 * overflow of the original's arithmetic) it is kept apart from the logic. */
#include "hook.h"

/* --- WRAM --- */
#define TileMap          0x7FC000   /* 2 bytes per cell, rows $80 apart */
#define CellItems        0x7FD000   /* per cell; $0E = special (guess) */
#define BlastMap         0x7F8000   /* per cell, $FFFF = reached by a blast */
#define TileGfxMap       0x7FA000   /* per-cell graphics codes (guess) */
#define TileGfxMap2      0x7FB000
#define StageOriginX     0x7E5F01
#define StageOriginY     0x7E5EFB
#define GoldPanels       0x7E5B56   /* 8-byte entries: +0 state, +2 cell, +4 gfx */
#define Exit_PanelState  0x7E5D94
#define BlastStopped     0x7E5DAE
#define BlastHitSpecial  0x7E5E46
#define Player1          0x7E8000
#define PasswordDigits   0x7F0028   /* 4 digits, one per word */
#define PasswordResult   0x7F0030
#define PasswordIndex    0x001C6A

/* Direct page (all hooks run with D = 0). */
#define dp_TileX     0x4A
#define dp_TileY     0x4C
#define dp_TileIndex 0x4E
#define dp_PlayerCell 0x5E

/* Player object (+$100 per player). */
enum { OBJ_STATE60 = 0x60, OBJ_CELL = 0xA8 };
/* Bomb object ($7F2000 + $100 * n). */
enum { BOMB_POS = 0x02, BOMB_RANGE = 0x07, BOMB_CELL = 0x0A };

/* --- carry/overflow bookkeeping for exact flag outputs --- */
static int fc, fv;

static void load_cv(void) { fc = GetFlag(FC); fv = GetFlag(FV); }
static void store_cv(void) { SetFlag(FC, fc); SetFlag(FV, fv); }

static uint16_t adc16(uint16_t a, uint16_t b, int carry)
{
    uint32_t r = (uint32_t)a + b + carry;
    fc = r > 0xFFFF;
    fv = (~(a ^ b) & (a ^ r) & 0x8000) != 0;
    return r;
}
static uint16_t sbc16(uint16_t a, uint16_t b, int carry) { return adc16(a, ~b, carry); }
/* cmp: carry = no borrow; returns the difference for N/Z. */
static uint16_t cmp16(uint16_t a, uint16_t b) { fc = a >= b; return a - b; }
static uint8_t cmp8(uint8_t a, uint8_t b) { fc = a >= b; return a - b; }

/* C0:BFE2  Cell index of pixel ($4A, $4C) + stage origin -> $4E; returns
 * the tile in A, the wrapped index in X. */
static void MapTileAtXY(void)
{
    uint16_t row = (uint16_t)((ram16(dp_TileY) + ram16(StageOriginY)) & 0x3F0) << 3;
    uint16_t col = ((ram16(dp_TileX) + ram16(StageOriginX)) & 0x3F0) >> 3;
    uint16_t index = adc16(col, row, 0);
    ram16_set(dp_TileIndex, index);
    uint16_t x = index;
    cmp16(index & 0x7F, 0x40);
    if (fc) x = index & 0xFFBF;     /* columns 32-63 fold onto 0-31 */
    x &= 0x0FFF;
    g_cpu->x = x;
    g_cpu->a = PeekLong16(TileMap + x);
    SetNZ16(g_cpu->a);
    SetFlag(FC, fc);
    SetFlag(FV, fv);
    Ret_RTL();
}

/* C1:4031  Runs the handler for the tile under player 1 (table C1:58DE,
 * 4 bytes per tile type), or only the exit check while +$60 is negative. */
static void PlayerTileHandler(void)
{
    Cpu *c = g_cpu;
    ram8_set(0x6A, 0x00);   /* [$6A] = player 1 object, as handlers expect */
    ram8_set(0x6B, 0x80);
    ram8_set(0x6C, 0x7E);
    SetM8(0);
    uint16_t cell = ram16(Player1 + OBJ_CELL);
    uint16_t type = PeekLong16(TileMap + cell) & 0x3F;
    c->y = OBJ_CELL;
    if (ram16(Player1 + OBJ_STATE60) & 0x8000) {
        c->x = cell;
        c->a = type;
        SetNZ16(cmp16(type, 9));
        SetFlag(FC, fc);
        if (type == 9) JumpLong(0xC141A8);   /* PlayerOnExitTile */
        else Ret_RTL();
        return;
    }
    uint32_t entry = 0xC158DE + type * 4;
    uint8_t lo = PeekLong8(entry), hi = PeekLong8(entry + 1), bank = PeekLong8(entry + 2);
    ram8_set(0x7F, lo);
    ram8_set(0x80, hi);
    ram8_set(0x81, bank);
    c->x = type * 4 + 2;
    c->a = bank;
    SetNZ8(bank);
    SetFlag(FC, 0);
    JumpLong((uint32_t)bank << 16 | hi << 8 | lo);   /* jml [$7F] */
}

/* C1:2335  Sets bit 15 of Exit_PanelState once every gold panel is done:
 * mode 0 (bit 0 clear) when all their cells show tile $70, mode 1 when all
 * entries have state bit 0 set. */
static void CheckGoldPanels(void)
{
    Cpu *c = g_cpu;
    load_cv();
    c->a = ram16(GoldPanels);
    if (c->a & 0x8000) { SetNZ16(c->a); Ret_RTL(); return; }
    c->a = ram16(Exit_PanelState);
    if (c->a & 0x8000) { SetNZ16(c->a); Ret_RTL(); return; }
    ram8_set(0x97, 0x56);
    ram8_set(0x98, 0x5B);
    ram8_set(0x99, 0x7E);
    uint16_t p = 0x5B56;
    int mode1 = ram16(Exit_PanelState) & 1;
    for (;;) {
        uint16_t state = ram16(0x7E0000 | p);
        uint16_t d = cmp16(state, 0xFFFF);
        if (state == 0xFFFF) break;
        if (mode1) {
            if (!(state & 1)) {      /* bit #1 leaves N from the cmp */
                c->a = state;
                SetFlag(FN, d & 0x8000);
                SetFlag(FZ, 1);
                store_cv();
                Ret_RTL();
                return;
            }
        } else {
            c->y = 2;
            c->x = ram16(0x7E0000 | (uint16_t)(p + 2));
            c->a = PeekLong16(TileMap + c->x);
            if (c->a != 0x70) {
                SetNZ16(cmp16(c->a, 0x70));
                store_cv();
                Ret_RTL();
                return;
            }
            cmp16(c->a, 0x70);
        }
        p = adc16(p, 8, 0);
        ram16_set(0x97, p);
    }
    c->a = ram16(Exit_PanelState) | 0x8000;
    ram16_set(Exit_PanelState, c->a);
    SetNZ16(c->a);
    store_cv();
    Ret_RTL();
}

/* C1:2282  In panel mode 1, standing on a panel ($73/$74) flips the state
 * of the gold panel entry at the player's cell and redraws it. */
static void GoldPanelToggle(void)
{
    Cpu *c = g_cpu;
    load_cv();
    c->a = ram16(GoldPanels);
    if (c->a & 0x8000) { SetNZ16(c->a); Ret_RTL(); return; }
    c->a = ram16(Exit_PanelState);
    SetNZ16(c->a);
    if (c->a & 0x8000) { Ret_RTL(); return; }
    if (!(c->a & 1)) { SetFlag(FZ, 1); Ret_RTL(); return; }   /* bit #1 */
    uint16_t cell = ram16(dp_PlayerCell);
    c->x = cell;
    c->a = PeekLong16(TileMap + cell);
    uint16_t d = cmp16(c->a, 0x73);
    if (c->a != 0x73) {
        d = cmp16(c->a, 0x74);
        if (c->a != 0x74) { SetNZ16(d); store_cv(); Ret_RTL(); return; }
    }
    ram8_set(0x97, 0x56);
    ram8_set(0x98, 0x5B);
    ram8_set(0x99, 0x7E);
    uint16_t p = 0x5B56;
    for (;;) {
        uint32_t e = 0x7E0000 | p;
        uint16_t state = ram16(e);
        cmp16(state, 0xFFFF);
        if (state == 0xFFFF) break;
        c->y = 2;
        c->a = ram16(e + 2);
        d = cmp16(c->a, cell);
        if (c->a == cell) {
            uint16_t gfx = (state & 1) ? 0x1248 : 0x1246;
            uint16_t target = ram16(e + 4);
            WriteLong16(TileGfxMap + target, gfx);
            WriteLong16(TileGfxMap2 + target, gfx);
            state ^= 1;
            ram16_set(e, state);
            c->x = target;
            c->y = gfx;
        }
        p = adc16(p, 8, 0);
        ram16_set(0x97, p);
    }
    c->a = 0xFFFF;
    SetNZ16(0);
    store_cv();
    Ret_RTL();
}

/* --- multiply helpers and the RNG --- */

/* C0:0D20  $44 = $40 * $42 (low 16 bits), shift-and-add from the top bit of
 * $40; leaves $40 = 0, Y = 0 and A = the last partial sum. */
static void Mul16x16_core(void)
{
    uint16_t acc = ram16(0x44), m = ram16(0x40), k = ram16(0x42);
    for (int i = 0; i < 16; i++) {
        acc <<= 1;
        fc = m >> 15;
        m <<= 1;
        if (fc) g_cpu->a = acc = adc16(k, acc, 0);
    }
    ram16_set(0x44, acc);
    ram16_set(0x40, m);
    g_cpu->y = 0;
    SetNZ16(0);
}

static void Mul16x16(void)
{
    load_cv();
    Mul16x16_core();
    store_cv();
    Ret_RTL();
}

/* C0:0D02  $40:$44 = (high byte of $D8) * $42, MSB first; $D8 <<= 8. */
static void Mul8x16_core(void)
{
    uint16_t lo = 0, hi = 0, d8 = ram16(0xD8), k = ram16(0x42);
    for (int i = 0; i < 8; i++) {
        hi = hi << 1 | lo >> 15;
        lo <<= 1;
        fc = d8 >> 15;
        d8 <<= 1;
        if (fc) {
            g_cpu->a = lo = adc16(k, lo, 0);
            if (fc) hi++;
        }
    }
    ram16_set(0x44, lo);
    ram16_set(0x40, hi);
    ram16_set(0xD8, d8);
    g_cpu->y = 0;
    SetNZ16(0);
}

static void Mul8x16(void)
{
    load_cv();
    Mul8x16_core();
    store_cv();
    Ret_RTL();
}

/* C0:0CDF  Random: seed $D6 = ($D6 | 1) * $383; returns in A (and $40)
 * the high word of (low byte of X) * seed. Callers pass the old seed in X. */
static void Random(void)
{
    Cpu *c = g_cpu;
    load_cv();
    SetM8(0);
    ram16_set(0x40, ram16(0xD6) | 1);
    ram16_set(0x42, 0x0383);
    c->a = 0x0383;
    Mul16x16_core();
    uint16_t seed = ram16(0x44);
    ram16_set(0x42, seed);
    ram16_set(0xD6, seed);
    ram16_set(0xD8, (uint16_t)(c->x >> 8 | c->x << 8));
    Mul8x16_core();
    c->a = ram16(0x40);
    SetNZ16(c->a);
    store_cv();
    Ret_RTL();
}

/* --- bomb blasts ---
 * C0:323F/32E5/3386/3423 extend a bomb's blast up, down, left and right:
 * for range+1 steps it appends {pixel pos, cell, segment type} entries to
 * the bomb's list for that direction and marks reached cells in BlastMap,
 * until a solid tile (bit 7), a $43/$44 tile or the top of the map stops
 * it (later entries are still written). The last entry gets the end type.
 * In: [$7C] = bomb object. */
typedef struct BlastDir {
    uint16_t list;        /* offset of the direction's entry list in the bomb */
    int16_t dpos, dcell;  /* step in pixels-ish units and in cell index */
    uint16_t segment, end;
    uint8_t arith;        /* how the original steps: 0 sbc, 1 adc, 2 dec/inc */
    uint8_t top_check;    /* stop at cells >= $2000 (wrapped above row 0) */
} BlastDir;

static void BlastExtend(const BlastDir *dir)
{
    Cpu *c = g_cpu;
    load_cv();
    ram16_set(BlastStopped, 0);
    uint32_t bomb = ram16(0x7C) | (uint32_t)ram8(0x7E) << 16;
    ram8_set(0x7B, ram8(0x7E));
    uint8_t range = ReadLong8(bomb + BOMB_RANGE);
    uint16_t steps = range + 1;
    uint16_t pos = ReadLong16(bomb + BOMB_POS);
    uint16_t cell = ReadLong16(bomb + BOMB_CELL);
    uint16_t entry = adc16(bomb, dir->list, 0);
    uint32_t bank = bomb & 0xFF0000;
    for (;;) {
        if (dir->arith == 2) {
            pos += dir->dpos;
            cell += dir->dcell;
        } else if (dir->arith == 0) {
            pos = sbc16(pos, -dir->dpos, 1);
            cell = sbc16(cell, -dir->dcell, 1);
        } else {
            pos = adc16(pos, dir->dpos, 0);
            cell = adc16(cell, dir->dcell, 0);
        }
        WriteLong16(bank | entry, pos);
        WriteLong16((bank | entry) + 2, cell);
        WriteLong16((bank | entry) + 4, dir->segment);
        if (!ram16(BlastStopped)) {
            c->x = cell;
            int stop = 0, skip = 0;
            if (dir->top_check) { cmp16(cell, 0x2000); skip = fc; }
            if (!skip) {
                uint16_t tile = ReadLong16(TileMap + cell);
                if (tile & 0x80) stop = 1;
                else if (cmp16(tile, 0x44), tile == 0x44) stop = 1;
                else if (cmp16(tile, 0x43), tile == 0x43) stop = 1;
                if (stop) {
                    ram16_set(BlastStopped, 0xFFFF);
                } else {
                    cmp16(ReadLong16(CellItems + cell), 0x000E);
                    if (ReadLong16(CellItems + cell) == 0x000E) ram16_set(BlastHitSpecial, 0xFFFF);
                    WriteLong16(BlastMap + cell, 0xFFFF);
                }
            }
        }
        if (--steps == 0) break;
        entry = adc16(entry, 6, 0);
    }
    ram16_set(0x40, pos);
    ram16_set(0x42, cell);
    ram16_set(0x4E, 0);
    ram16_set(0x79, entry);
    WriteLong16((bank | entry) + 4, dir->end);
    c->a = dir->end;
    c->y = 4;
    SetNZ16(c->a);
    store_cv();
    Ret_RTL();
}

static const BlastDir kBlastUp    = { 0x10, -0x40, -0x80, 5, 1, 0, 1 };
static const BlastDir kBlastDown  = { 0x4C, 0x40, 0x80, 5, 2, 1, 0 };
static const BlastDir kBlastLeft  = { 0x88, -2, -2, 6, 3, 2, 0 };
static const BlastDir kBlastRight = { 0xC4, 2, 2, 6, 4, 2, 0 };

static void BlastExtendUp(void) { BlastExtend(&kBlastUp); }
static void BlastExtendDown(void) { BlastExtend(&kBlastDown); }
static void BlastExtendLeft(void) { BlastExtend(&kBlastLeft); }
static void BlastExtendRight(void) { BlastExtend(&kBlastRight); }

/* --- password --- */

/* CC:1926  Looks the 4 entered digits up in the 11-entry table at CC:0000
 * (4 bytes each). Found: PasswordIndex = entry, result 1, CC:8193. Not
 * found: result 2, CC:81C0, digits cleared, CC:1AA0. Result goes to
 * $7F0030 + X (the caller's X). */
static void CheckPassword(void)
{
    Cpu *c = g_cpu;
    load_cv();
    SetM8(0);
    ram16_set(0x76, 0x0000);
    ram8_set(0x78, 0xCC);
    for (uint16_t i = 0; i < 11; i++) {
        ram16_set(0x46, i);
        uint16_t y = i * 4;
        int j;
        for (j = 0; j < 4; j++) {
            uint8_t want = PeekLong8(0xCC0000 + y + j);
            cmp8(want, ram8(PasswordDigits + 2 * j));
            if (want != ram8(PasswordDigits + 2 * j)) break;
        }
        c->y = y + (j < 4 ? j : 3);
        if (j == 4) {
            ram16_set(PasswordIndex, i);
            c->a = 1;
            SetNZ16(1);
            store_cv();
            WriteLong16(PasswordResult + c->x, 1);
            CallLong(0xCC8193);
            Ret_RTL();
            return;
        }
    }
    ram16_set(0x46, 11);
    fc = 1;
    c->a = 2;
    SetNZ16(2);
    store_cv();
    WriteLong16(PasswordResult + c->x, 2);
    CallLong(0xCC81C0);
    SetM8(0);
    c->a = 0;
    for (int j = 0; j < 4; j++) ram16_set(PasswordDigits + 2 * j, 0);
    for (int j = 0; j < 4; j++) ram16_set(0x001C60 + 2 * j, 0);
    SetNZ16(0);
    CallLong(0xCC1AA0);
    Ret_RTL();
}

const Hook g_hooks[] = {
    { 0xC0BFE2, "MapTileAtXY",       MapTileAtXY,       FM | FX, 0, 0 },
    { 0xC14031, "PlayerTileHandler", PlayerTileHandler, FX,      0, 0 },
    { 0xC12335, "CheckGoldPanels",   CheckGoldPanels,   FM | FX, 0, 0 },
    { 0xC12282, "GoldPanelToggle",   GoldPanelToggle,   FM | FX, 0, 0 },
    { 0xC00D20, "Mul16x16",          Mul16x16,          FM | FX, 0, 0 },
    { 0xC00D02, "Mul8x16",           Mul8x16,           FM | FX, 0, 0 },
    { 0xC00CDF, "Random",            Random,            FX,      0, 0 },
    { 0xC0323F, "BlastExtendUp",     BlastExtendUp,     FM | FX, 0, 0 },
    { 0xC032E5, "BlastExtendDown",   BlastExtendDown,   FM | FX, 0, 0 },
    { 0xC03386, "BlastExtendLeft",   BlastExtendLeft,   FM | FX, 0, 0 },
    { 0xC03423, "BlastExtendRight",  BlastExtendRight,  FM | FX, 0, 0 },
    { 0xCC1926, "CheckPassword",     CheckPassword,     FX,      0, 0 },
};
const int g_nhooks = sizeof g_hooks / sizeof *g_hooks;
