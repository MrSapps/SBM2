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
#define ScoreHundreds    0x7E5DEE   /* score / 100 */
#define Exit_Blocked     0x7E5D70
#define ExitGateCell     0x7E5D72

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
static uint16_t MapTileAtXY_core(void)
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
    g_cpu->a = ram16(TileMap + x);
    SetNZ16(g_cpu->a);
    return g_cpu->a;
}

static void MapTileAtXY(void)
{
    load_cv();
    MapTileAtXY_core();
    store_cv();
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
static void GoldPanelToggle_core(void)
{
    Cpu *c = g_cpu;
    load_cv();
    c->a = ram16(GoldPanels);
    if (c->a & 0x8000) { SetNZ16(c->a); return; }
    c->a = ram16(Exit_PanelState);
    SetNZ16(c->a);
    if (c->a & 0x8000) { return; }
    if (!(c->a & 1)) { SetFlag(FZ, 1); return; }   /* bit #1 */
    uint16_t cell = ram16(dp_PlayerCell);
    c->x = cell;
    c->a = PeekLong16(TileMap + cell);
    uint16_t d = cmp16(c->a, 0x73);
    if (c->a != 0x73) {
        d = cmp16(c->a, 0x74);
        if (c->a != 0x74) { SetNZ16(d); store_cv(); return; }
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
}


static void GoldPanelToggle(void)
{
    GoldPanelToggle_core();
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

static void BlastExtend_core(const BlastDir *dir)
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
}

static void BlastExtend(const BlastDir *dir) { BlastExtend_core(dir); Ret_RTL(); }

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


/* --- object sprites ---
 * [$6A] = object, [$79] = sprite piece {dx, dy, tile, attr, ...}. Object
 * fields: +$80 OAM slot, +$82/+$84 screen x/y, +$86 tile + attr (word),
 * +$88 added to the attr byte, +$06 palette (bits 1-3 of attr) unless
 * negative, +$04 bit 6 = hidden, +$52 bit 4 (with scene $1A&$0F = 7) and
 * scene group $40 force priority bits. */
#define OamBuffer 0x7E2200      /* 4 bytes per slot */
#define OamHigh   0x7E2400      /* 2 bits per slot */

static uint32_t dp_ptr(uint8_t dp) { return ram16(dp) | (uint32_t)ram8(dp + 2) << 16; }
static uint8_t ind8(uint8_t dp, uint16_t y) { return PeekLong8(dp_ptr(dp) + y); }
static uint16_t ind16(uint8_t dp, uint16_t y) { return PeekLong16(dp_ptr(dp) + y); }
static void ind8_set(uint8_t dp, uint16_t y, uint8_t v) { WriteLong8(dp_ptr(dp) + y, v); }
static void ind16_set(uint8_t dp, uint16_t y, uint16_t v) { WriteLong16(dp_ptr(dp) + y, v); }

/* C0:A077  Screen position = object x/y + the piece's signed dx/dy. Leaves
 * $4E = dy (word), A = y, Y = $84, C/V from the add, N/Z from Y. */
static void SpritePieceOffset_core(void)
{
    Cpu *c = g_cpu;
    uint16_t dx = (int8_t)ind8(0x79, 0);
    ram16_set(0x4E, dx);
    ind16_set(0x6A, 0x82, adc16(ind16(0x6A, 0x10), dx, 0));
    uint16_t dy = (int8_t)ind8(0x79, 1);
    ram16_set(0x4E, dy);
    c->a = adc16(ind16(0x6A, 0x18), dy, 0);
    ind16_set(0x6A, 0x84, c->a);
    c->y = 0x84;
    SetNZ16(c->y);   /* ldy #$0084 is the last flag setter */
}

static void SpritePieceOffset(void)
{
    load_cv();
    SpritePieceOffset_core();
    store_cv();
    Ret_RTL();
}

/* C0:9FFB  Priority bits $30 into attr (+$86) in scene group $40, or in
 * scene type 7 while +$52 bit 4 is set. Not while $B9 is negative. */
static void SpritePriority_core(void)
{
    Cpu *c = g_cpu;
    c->a = ram16(0xB9);
    if (c->a & 0x8000) { SetNZ16(c->a); return; }
    uint16_t a = ram16(0x1A) & 0xF0;
    if (a != 0x40) {
        uint16_t t = ram16(0x1A) & 0x0F;
        if (t != 7) { c->a = t; SetNZ16(cmp16(t, 7)); return; }
        fc = 1;
        c->y = 0x52;
        c->a = ind16(0x6A, 0x52);
        if (!(c->a & 0x10)) { SetFlag(FN, c->a & 0x8000); SetFlag(FZ, 1); return; }
    } else {
        fc = 1;
        c->a = a;
    }
    c->y = 0x86;
    uint8_t v = ind8(0x6A, 0x86) | 0x30;
    ind8_set(0x6A, 0x86, v);
    c->a = (c->a & 0xFF00) | v;
    SetNZ8(v);
}

static void SpritePriority(void)
{
    load_cv();
    SpritePriority_core();
    SetFlag(FC, fc);
    Ret_RTL();
}

/* C0:A02D  Tile (+$86) and attr (+$87) from the piece, palette from +$06. */
static void SpritePieceAttr_core(void)
{
    Cpu *c = g_cpu;
    uint8_t tile = ind8(0x79, 2);
    ram8_set(0x4E, tile);
    c->y = 0x86;
    ind8_set(0x6A, 0x86, (ind8(0x79, 3) & 1) | tile);
    SpritePriority_core();
    c->y = 0x87;
    ind8_set(0x6A, 0x87, ind8(0x79, 4));
    c->y = 6;
    uint16_t pal = ind16(0x6A, 6);
    if (pal & 0x8000) { c->a = pal; SetNZ16(pal); return; }
    uint8_t shifted = (uint8_t)(pal << 1);
    fc = (pal >> 7) & 1;
    ram8_set(0x4E, shifted);
    c->y = 0x86;
    uint8_t v = (ind8(0x6A, 0x86) & 0xF1) | shifted;
    ind8_set(0x6A, 0x86, v);
    c->a = (pal & 0xFF00) | v;
    SetNZ8(v);
}

static void SpritePieceAttr(void)
{
    load_cv();
    SpritePieceAttr_core();
    SetFlag(FC, fc);
    Ret_RTL();
}

/* C0:A0B8  The slot's two OAM high-table bits: bit 0 = x bit 8 (+$82 bit 8),
 * bit 1 = size (piece attr bit 7), shifted into place in $7E2400 using the
 * mask table at C5:FBEB. Returns with A/X/Y 16-bit. */
static void SpriteHighBits_core(void)
{
    Cpu *c = g_cpu;
    uint8_t bits = 0;
    if (ind16(0x6A, 0x82) & 0x100) bits |= 1;
    if (ind8(0x79, 3) & 0x80) bits |= 2;
    uint16_t slot = ind16(0x6A, 0x80);
    ram16_set(0x48, slot);
    uint16_t a16 = slot >> 2;
    uint8_t xi = a16 & 0xFF;
    uint8_t old = ram8(OamHigh + xi);
    ram8_set(0x40, old);
    uint8_t sub = slot & 3;
    uint8_t mask = PeekLong8(0xC5FBEB + sub);
    ram8_set(0x44, mask);
    for (int i = 0; i < sub * 2; i++) bits <<= 1;
    ram16_set(0x46, bits);
    ram8_set(0x42, 0);
    uint8_t v = (old & mask) | bits;
    uint8_t lo = slot & 0xFF;
    fc = (lo >> 1) & 1;
    xi = lo >> 2;
    ram8_set(OamHigh + xi, v);
    c->x = xi;
    c->y = v;
    c->a = (a16 & 0xFF00) | v;
    SetNZ8(v);
    SetM8(0);
    SetX8(0);
}

static void SpriteHighBits(void)
{
    load_cv();
    SpriteHighBits_core();
    SetFlag(FC, fc);
    Ret_RTL();
}

/* C1:4E1E  Builds an object's OAM entry from its current sprite piece. */
static void DrawObjectSprite(void)
{
    Cpu *c = g_cpu;
    load_cv();
    SpritePieceOffset_core();
    int v_offset = fv;
    SpritePieceAttr_core();
    SpriteHighBits_core();
    uint16_t slot = ind16(0x6A, 0x80);
    uint16_t x = slot << 2;
    c->x = x;
    c->y = 4;
    uint16_t flags = ind16(0x6A, 4);
    if (flags & 0x40) {             /* hidden */
        c->a = flags;
        SetFlag(FN, flags & 0x8000);
        SetFlag(FZ, 0);
        SetFlag(FC, (slot >> 14) & 1);
        SetFlag(FV, v_offset);
        Ret_RTL();
        return;
    }
    uint16_t tw = ind16(0x6A, 0x86);
    uint16_t sum = adc16((uint16_t)(tw << 8 | tw >> 8), ind16(0x6A, 0x88), 0);
    uint16_t nw = sum << 8 | sum >> 8;
    ind16_set(0x6A, 0x86, nw);
    WriteLong8(OamBuffer + x, ind8(0x6A, 0x82));
    WriteLong8(OamBuffer + x + 1, ind8(0x6A, 0x84));
    WriteLong8(OamBuffer + x + 2, nw >> 8);
    WriteLong8(OamBuffer + x + 3, nw & 0xFF);
    c->x = x + 3;
    c->y = 0x86;
    c->a = nw;
    SetNZ8(nw & 0xFF);
    store_cv();
    Ret_RTL();
}

/* C0:F117  If neither $7F6000[$46] nor [$44] has a bit of mask $42, ors the
 * object's +[$7C] / +[$79] (masked) into them. Works at either width (the
 * callers use 16-bit A, mask $0101 shifted per call: two bytes at a time).
 * (Purpose unclear: a per-cell bit map at $7F6000.) */
static uint16_t rdw(uint32_t a, int m16) { return m16 ? ReadLong16(a) : ReadLong8(a); }
static void wrw(uint32_t a, uint16_t v, int m16) { if (m16) WriteLong16(a, v); else WriteLong8(a, v); }

static void MergeCellMask(void)
{
    Cpu *c = g_cpu;
    int x16 = !GetFlag(FX), m16 = !GetFlag(FM);
    uint16_t keep = m16 ? 0 : c->a & 0xFF00;
    uint16_t i46 = x16 ? ram16(0x46) : ram8(0x46);
    uint16_t i44 = x16 ? ram16(0x44) : ram8(0x44);
    uint16_t mask = m16 ? ram16(0x42) : ram8(0x42);
    uint32_t obj = dp_ptr(0x6A);
    uint16_t b46 = rdw(0x7F6000 + i46, m16);
    wrw(0x76, b46, m16);
    c->x = i46;
    uint16_t t = b46 & mask;
    if (!t) {
        uint16_t b44 = rdw(0x7F6000 + i44, m16);
        wrw(0x73, b44, m16);
        c->x = i44;
        t = b44 & mask;
        if (!t) {
            uint16_t y7c = x16 ? ram16(0x7C) : ram8(0x7C);
            uint16_t y79 = x16 ? ram16(0x79) : ram8(0x79);
            wrw(0x7F6000 + i46, (rdw(obj + y7c, m16) & mask) | b46, m16);
            uint16_t v = (rdw(obj + y79, m16) & mask) | b44;
            wrw(0x7F6000 + i44, v, m16);
            c->a = keep | v;
            c->y = y79;
            if (x16) SetNZ16(i44); else SetNZ8(i44);
            SetM8(0);
            Ret_RTL();
            return;
        }
    }
    c->a = keep | t;
    if (m16) SetNZ16(t); else SetNZ8(t);
    SetM8(0);
    Ret_RTL();
}


/* --- map scans (stage setup) --- */

/* C0:A515  Graphics-map index of pixel ($4A, $4C) + stage origin -> $46
 * (and A); $4E/$48 = the wrapped y/x. */
static uint16_t TileGfxIndexAtXY_core(void)
{
    uint16_t y = adc16(ram16(StageOriginY), ram16(dp_TileY), 0) & 0x3F0;
    ram16_set(0x4E, y);
    uint16_t x = adc16(ram16(StageOriginX), ram16(dp_TileX), 0) & 0x3F0;
    ram16_set(0x48, x);
    uint16_t row = (y & 0x1F0) << 2;
    ram16_set(0x46, row);
    uint16_t idx = adc16((x & 0x1F0) >> 3, row, 0) & 0x7FF;
    ram16_set(0x46, idx);
    g_cpu->a = idx;
    SetNZ16(idx);
    return idx;
}

static void TileGfxIndexAtXY(void)
{
    load_cv();
    TileGfxIndexAtXY_core();
    store_cv();
    Ret_RTL();
}

/* C0:37E2 (map = TileMap) / C0:3778 (map = CellItems)  Lists every cell
 * of the 32 x 32 map whose code is $94 into [$97] as {cell, gfx index}
 * pairs, $FFFF-terminated; $50 = list size in bytes. */
static void FindInMap_core(uint32_t map)
{
    Cpu *c = g_cpu;
    uint16_t want = ram16(0x94), n = 0;
    ram16_set(0x5E, 0x20);
    ram16_set(0x58, 0x20);
    ram16_set(0x5C, 0);
    ram16_set(0x5A, 0);
    for (uint16_t rows = 0x20; rows; ) {
        for (uint16_t cols = 0x20; cols; ) {
            ram16_set(dp_TileY, ram16(0x5C));
            ram16_set(dp_TileX, ram16(0x5A));
            MapTileAtXY_core();
            ram16_set(0x48, c->x);
            if (ram16(map + c->x) == want) {
                ind16_set(0x97, n, ram16(0x48));
                n += 2;
                ram16_set(dp_TileY, ram16(0x5C));
                ram16_set(dp_TileX, ram16(0x5A));
                TileGfxIndexAtXY_core();
                ind16_set(0x97, n, ram16(0x46));
                n += 2;
            }
            ram16_set(0x5A, adc16(ram16(0x5A), 0x10, 0));
            ram16_set(0x58, --cols);
        }
        ram16_set(0x58, 0x20);
        ram16_set(0x5C, adc16(ram16(0x5C), 0x10, 0));
        ram16_set(0x5A, 0);
        ram16_set(0x5E, --rows);
    }
    ram16_set(0x50, n);
    c->a = 0xFFFF;
    c->y = n;
    ind16_set(0x97, n, 0xFFFF);
    SetNZ16(n);
}

static void FindTileInMap(void) { load_cv(); FindInMap_core(TileMap); store_cv(); Ret_RTL(); }
static void FindItemInMap(void) { load_cv(); FindInMap_core(CellItems); store_cv(); Ret_RTL(); }

/* C1:2558  Gold panels are tile $69 cells (mode 0) or $74 (mode 1); their
 * list goes to $7F6000, then C1:25B3 builds GoldPanels. None: both $FFFF. */
static void FindGoldPanels(void)
{
    Cpu *c = g_cpu;
    load_cv();
    static const uint16_t kTiles[2] = { 0x69, 0x74 };
    for (int mode = 0; mode < 2; mode++) {
        ram16_set(0x94, kTiles[mode]);
        ram16_set(0x97, 0x6000);
        ram8_set(0x99, 0x7F);
        FindInMap_core(TileMap);
        c->a = 0x607F;
        SetNZ8(0x7F);
        uint16_t first = ram16(0x7F6000);
        cmp16(first, 0xFFFF);
        if (first != 0xFFFF) {
            c->a = mode;
            ram16_set(Exit_PanelState, mode);
            SetNZ16(mode);
            store_cv();
            CallLong(0xC125B3);
            Ret_RTL();
            return;
        }
    }
    c->a = 0xFFFF;
    ram16_set(GoldPanels, 0xFFFF);
    ram16_set(Exit_PanelState, 0xFFFF);
    SetNZ16(0xFFFF);
    store_cv();
    Ret_RTL();
}

/* C1:2A9C  The exit gate is the CellItems $C9 cell; its object: C0:EE1C. */
static void FindExitGate(void)
{
    Cpu *c = g_cpu;
    load_cv();
    ram16_set(0x94, 0xC9);
    ram16_set(0x97, 0x30F0);
    ram8_set(0x99, 0x7E);
    FindInMap_core(CellItems);
    c->a = ram16(0x7E30F0);
    ram16_set(ExitGateCell, c->a);
    SetNZ16(c->a);
    store_cv();
    CallLong(0xC0EE1C);
    Ret_RTL();
}

/* --- enemies --- */

/* C1:5028  Enemy [$6A] dies: state/anim reset, +4 |= $A000, y = 0. Gives
 * the score from table C5:D412 indexed by +$44, except for types $90/$98,
 * which instead restore the item under them (CellItems -> TileMap). */
static void EnemyDie(void)
{
    Cpu *c = g_cpu;
    load_cv();
    ind16_set(0x6A, 0x50, 0);
    ind16_set(0x6A, 0x6A, 0x0C);
    ind16_set(0x6A, 0x68, ind16(0x6A, 0x68) & 0xFFF0);
    ind16_set(0x6A, 0x04, ind16(0x6A, 0x04) & 0xFFDF);
    ind16_set(0x6A, 0x06, ind16(0x6A, 0x2A));
    ind16_set(0x6A, 0x88, ind16(0x6A, 0x8A));
    ind16_set(0x6A, 0x04, ind16(0x6A, 0x04) | 0xA000);
    ind16_set(0x6A, 0x18, 0);
    uint16_t type = ind16(0x6A, 0x7E);
    if (type == 0x90 || type == 0x98) {
        fc = 1;
        c->y = 0xA8;
        c->x = ind16(0x6A, 0xA8);
        c->a = ram16(CellItems + c->x);
        ram16_set(TileMap + c->x, c->a);
        SetNZ16(c->a);
    } else {
        c->y = 0x44;
        c->x = ind16(0x6A, 0x44);
        c->a = adc16(PeekLong16(0xC5D412 + c->x), ram16(ScoreHundreds), 0);
        ram16_set(ScoreHundreds, c->a);
        SetNZ16(c->a);
    }
    store_cv();
    Ret_RTL();
}


/* C0:A570  Object [$6A] flashing (+4 bit 5): +$B9 counts down ($FF = no
 * limit); while it runs, palette +6 toggles between 7 and +$2A on even
 * frames; at 0 the palette is restored and the flag cleared. */
static void ObjFlashTick(void)
{
    Cpu *c = g_cpu;
    load_cv();
    c->y = 4;
    uint16_t f = ind16(0x6A, 4);
    if (!(f & 0x20)) { c->a = f; SetFlag(FN, f & 0x8000); SetFlag(FZ, 1); SetM8(0); Ret_RTL(); return; }
    c->y = 0xB9;
    uint8_t t = ind8(0x6A, 0xB9);
    cmp8(t, 0xFF);
    int restore = 0;
    if (t != 0xFF) {
        if (--t == 0) restore = 1;
        else ind8_set(0x6A, 0xB9, t);
    }
    SetM8(0);
    if (restore) {
        ind16_set(0x6A, 6, ind16(0x6A, 0x2A));
        c->y = 4;
        c->a = ind16(0x6A, 4) & 0xFFDF;
        ind16_set(0x6A, 4, c->a);
        SetNZ16(c->a);
        store_cv();
        Ret_RTL();
        return;
    }
    c->a = ram16(0x32);
    if (c->a & 1) { SetFlag(FN, c->a & 0x8000); SetFlag(FZ, 0); store_cv(); Ret_RTL(); return; }
    uint16_t pal = ind16(0x6A, 6);
    cmp16(pal, 7);
    c->a = pal == 7 ? ind16(0x6A, 0x2A) : 7;
    c->y = 6;
    ind16_set(0x6A, 6, c->a);
    SetNZ16(c->y);   /* the last flag setter is ldy #$0006 */
    store_cv();
    Ret_RTL();
}

/* C1:1E6F  Enemy [$82] gets stunned unless already (+$BE bit 0 / negative)
 * or +$D0 & $0F: state reset, motion cleared, flashing without limit. */
static void EnemyStun(void)
{
    Cpu *c = g_cpu;
    c->y = 0xBE;
    uint16_t be = ind16(0x82, 0xBE);
    c->a = be;
    SetNZ16(be);
    if (be & 0x8000) { Ret_RTL(); return; }
    if (be & 1) { SetFlag(FZ, 0); Ret_RTL(); return; }
    c->y = 0xD0;
    c->a = ind16(0x82, 0xD0) & 0x0F;
    if (c->a) { SetNZ16(c->a); Ret_RTL(); return; }
    ind16_set(0x82, 0xBE, ind16(0x82, 0xBE) | 1);
    ind16_set(0x82, 0x50, 0);
    ind16_set(0x82, 0x6A, 4);
    ind16_set(0x82, 0x0C, 0);
    ind16_set(0x82, 0x68, (ind16(0x82, 0x68) & 0x7FFF) | 1);
    ind16_set(0x82, 0x14, 0);
    ind16_set(0x82, 0x16, 0);
    ind16_set(0x82, 0x1C, 0);
    ind16_set(0x82, 0x1E, 0);
    uint16_t f = ind16(0x82, 4) | 0x20;
    ind16_set(0x82, 4, f);
    ind8_set(0x82, 0xB9, 0xFF);
    c->y = 0xB9;
    c->a = (f & 0xFF00) | 0xFF;
    SetNZ8(0xFF);
    SetM8(0);
    Ret_RTL();
}

/* C1:1EE2  Enemy [$82] takes a hit (unless +$43 & $7F = $7F, or types
 * $A4/$A6 while +$50 is set): C1:1F41, then hit points +$43 down; at 0 it
 * is marked dying (+$68 bit 1) and flashes. */
static void EnemyHit(void)
{
    Cpu *c = g_cpu;
    load_cv();
    c->y = 0x43;
    uint16_t hp = ind8(0x82, 0x43) & 0x7F;
    if (hp == 0x7F) {
        c->a = 0x7F;
        SetNZ16(0);
        SetFlag(FC, 1);
        SetM8(0);
        Ret_RTL();
        return;
    }
    c->y = 0x7E;
    uint16_t type = ind16(0x82, 0x7E);
    c->a = type;
    if (type == 0xA4 || type == 0xA6) {
        fc = 1;
        c->y = 0x50;
        c->a = ind16(0x82, 0x50);
        if (c->a) { SetNZ16(c->a); store_cv(); SetM8(0); Ret_RTL(); return; }
    } else {
        cmp16(type, 0xA6);
    }
    SetNZ16(c->a);
    store_cv();
    SetM8(0);
    CallLong(0xC11F41);
    c->y = 0x43;
    uint8_t v = ind8(0x82, 0x43);
    if (v && --v) {
        ind8_set(0x82, 0x43, v);
        c->a = (c->a & 0xFF00) | v;
        SetNZ8(v);
        SetM8(0);
        Ret_RTL();
        return;
    }
    SetM8(0);
    ind16_set(0x82, 0x68, ind16(0x82, 0x68) | 2);
    uint16_t f = ind16(0x82, 4) | 0x20;
    ind16_set(0x82, 4, f);
    ind8_set(0x82, 0xB9, 0xFF);
    c->y = 0xB9;
    c->a = (f & 0xFF00) | 0xFF;
    SetNZ8(0xFF);
    Ret_RTL();
}


/* --- stage timer and score display ---
 * $7E5E10/$5E12 = minutes/seconds; $5E14 = frame counter for a second ($40
 * logic frames); $5E16..$5E1E = decimal digits scratch (10000s..1s);
 * $5E04/06/08 = timer digits m, s tens, s ones and $5E0A.. their gfx codes;
 * $5DF0..$5DF8 = score digits and $5DFA.. their gfx codes (word table at
 * C5:D432 per digit). */
#define TimerMinutes 0x7E5E10
#define TimerSeconds 0x7E5E12

/* C1:1CA3  Decimal digits of A by repeated subtraction. */
static void DigitSplit(uint16_t v)
{
    static const uint16_t kPow[4] = { 10000, 1000, 100, 10 };
    ram16_set(0x40, v);
    for (int i = 0; i < 5; i++) ram16_set(0x7E5E16 + 2 * i, 0);
    for (int i = 0; i < 4; i++) {
        for (;;) {
            uint16_t r = ram16(0x40) - kPow[i];
            if (r & 0x8000) break;
            ram16_set(0x40, r);
            ram16_set(0x7E5E16 + 2 * i, ram16(0x7E5E16 + 2 * i) + 1);
        }
    }
    ram16_set(0x7E5E1E, ram16(0x40));
}

static uint16_t digit_gfx(uint16_t d) { return PeekLong16(0xC5D432 + (uint16_t)(d << 1)); }

/* C1:1BA3  Score digits and their graphics. Called from the stage loops'
 * call lists, which ignore its registers. */
static void ScoreDigits(void)
{
    DigitSplit(ram16(ScoreHundreds));
    for (int i = 0; i < 5; i++) ram16_set(0x7E5DF0 + 2 * i, ram16(0x7E5E16 + 2 * i));
    for (int i = 0; i < 5; i++) ram16_set(0x7E5DFA + 2 * i, digit_gfx(ram16(0x7E5DF0 + 2 * i)));
    Ret_RTL();
}

/* C1:1C1A  Stage timer: unless stopped ($5E20 < 0, or $5C54 >= $0A / = 4
 * outside play), one second per $40 frames; at 0:00 $5C56 = 0 (time up).
 * Called from the stage loops' call lists, which ignore its registers. */
static void StageTimer(void)
{
    if (ram16(0x7E5E20) & 0x8000) { Ret_RTL(); return; }
    uint16_t mode = ram16(0x7E5C54);
    if ((ram16(0xB9) & 0x8000) ? mode == 4 : mode >= 0x0A) { Ret_RTL(); return; }
    uint16_t t = ram16(0x7E5E14);
    if (t != 0x40) { ram16_set(0x7E5E14, t + 1); Ret_RTL(); return; }
    ram16_set(0x7E5E14, 0);
    uint16_t sec = ram16(TimerSeconds) - 1;
    if (!(sec & 0x8000)) ram16_set(TimerSeconds, sec);
    else if (ram16(TimerMinutes)) {
        ram16_set(TimerMinutes, ram16(TimerMinutes) - 1);
        ram16_set(TimerSeconds, 59);
    } else ram16_set(0x7E5C56, 0);
    DigitSplit(ram16(TimerSeconds));
    ram16_set(0x7E5E04, ram16(TimerMinutes));
    ram16_set(0x7E5E06, ram16(0x7E5E1C));
    ram16_set(0x7E5E08, ram16(0x7E5E1E));
    for (int i = 0; i < 3; i++) ram16_set(0x7E5E0A + 2 * i, digit_gfx(ram16(0x7E5E04 + 2 * i)));
    Ret_RTL();
}


/* --- input ---
 * $C3/$C4 pad 1 ($C3 word), $C5-$C8 pads 2-4 low/high, $CD-$CF pad present
 * bits, $C9/$CB multitap pads 3/4 serial, $D0/$D1 their present bits, $C2
 * multitap flags ($80 port 1, $40 port 2). Player +$6E = filtered pad. */

/* C0:1797  Reads the auto-joypad registers and the multitap serial lines.
 * Saves/restores P; returns A = $0080, X = X & $FF, Y = 0. */
static void ReadJoypads_core(void)
{
    Cpu *c = g_cpu;
    ram8_set(0xC2, 0);
    while (ReadLong8(0x4212) & 1) {}
    ram8_set(0xC4, ReadLong8(0x4219));
    uint8_t p1 = ReadLong8(0x4218);
    ram8_set(0xC3, p1);
    ram8_set(0xCD, (uint8_t)((p1 & 0x0F) << 1 | (ReadLong8(0x4016) & 1)));
    ram8_set(0xC6, ReadLong8(0x421B));
    uint8_t p2 = ReadLong8(0x421A);
    ram8_set(0xC5, p2);
    uint8_t ce = p2 & 0x0F;
    ram8_set(0xC8, ReadLong8(0x421F));
    uint8_t p4 = ReadLong8(0x421E);
    ram8_set(0xC7, p4);
    uint8_t cf = p4 & 0x0F;
    uint8_t v = ReadLong8(0x4017);
    ram8_set(0xCE, (uint8_t)(ce << 1 | (v & 1)));
    ram8_set(0xCF, (uint8_t)(cf << 1 | (v >> 1 & 1)));
    WriteLong8(0x4201, 0);
    uint16_t c9 = ram16(0xC9), cb = ram16(0xCB);
    for (int i = 0; i < 16; i++) {
        v = ReadLong8(0x4017);
        c9 = c9 << 1 | (v & 1);
        cb = cb << 1 | (v >> 1 & 1);
    }
    ram16_set(0xC9, c9);
    ram16_set(0xCB, cb);
    v = ReadLong8(0x4017);
    ram8_set(0xD0, (uint8_t)((c9 & 0x0F) << 1 | (v & 1)));
    ram8_set(0xD1, (uint8_t)((cb & 0x0F) << 1 | (v >> 1 & 1)));
    WriteLong8(0x4201, 0x80);
    c->a = 0x0080;
    c->x &= 0xFF;
    c->y = 0;
}

static void ReadJoypads(void) { ReadJoypads_core(); Ret_RTL(); }

/* C0:1862  Multitap detection: strobes the pads and reads 16 bits of the
 * data-2 lines of both ports; all 1s then not = a tap. Saves/restores P. */
static void MultitapCheck_core(void)
{
    Cpu *c = g_cpu;
    ram8_set(0xC2, 0);
    while (ReadLong8(0x4212) & 1) {}
    WriteLong8(0x4016, 0);
    WriteLong8(0x4016, 1);
    uint8_t d3 = ram8(0xD3), d5 = ram8(0xD5), d2 = ram8(0xD2), d4 = ram8(0xD4);
    for (int i = 0; i < 8; i++) {
        d3 = (uint8_t)(d3 << 1 | (ReadLong8(0x4016) >> 1 & 1));
        d5 = (uint8_t)(d5 << 1 | (ReadLong8(0x4017) >> 1 & 1));
    }
    WriteLong8(0x4016, 0);
    for (int i = 0; i < 8; i++) {
        d2 = (uint8_t)(d2 << 1 | (ReadLong8(0x4016) >> 1 & 1));
        d4 = (uint8_t)(d4 << 1 | (ReadLong8(0x4017) >> 1 & 1));
    }
    ram8_set(0xD3, d3); ram8_set(0xD5, d5); ram8_set(0xD2, d2); ram8_set(0xD4, d4);
    uint8_t a;
    if (d3 == 0xFF && d2 != 0xFF) ram8_set(0xC2, 0x80);
    if (d5 != 0xFF) a = d5;
    else if (d4 == 0xFF) a = d4;
    else { a = 0x40 | ram8(0xC2); ram8_set(0xC2, a); }
    c->a = (c->a & 0xFF00) | a;
    c->x = 0;
    c->y &= 0xFF;
}

static void MultitapCheck(void) { MultitapCheck_core(); Ret_RTL(); }

/* C0:1625  Player 1's pad (when it can move and pad 1 alone is plugged in)
 * minus opposite directions held together -> $C3 and +$6E. */
static void FilterPlayer1Pad_core(void)
{
    Cpu *c = g_cpu;
    uint16_t v;
    if ((v = ram16(Player1)) & 0x0800) { c->a = v; SetFlag(FN, v & 0x8000); SetFlag(FZ, 0); return; }
    if ((v = ram16(Player1 + 4)) & 1) { c->a = v; SetFlag(FN, v & 0x8000); SetFlag(FZ, 0); return; }
    if ((v = ram16(Player1 + 0x52)) & 1) { c->a = v; SetFlag(FN, v & 0x8000); SetFlag(FZ, 0); return; }
    uint8_t cd = ram8(0xCD) & 0x1F;
    if (cd != 1) {
        c->a = (v & 0xFF00) | cd;
        SetNZ8(cmp8(cd, 1));
        SetFlag(FC, fc);
        return;
    }
    uint16_t pad = ram16(0xC3);
    ram16_set(0x40, pad);
    if ((pad & 0x0300) == 0x0300) pad &= 0xFCFF;   /* left + right */
    ram16_set(0x40, pad);
    cmp16(pad & 0x0C00, 0x0C00);
    if (fc) pad &= 0xF3FF;                         /* up + down */
    ram16_set(0x40, pad);
    ram16_set(0xC3, pad);
    ram16_set(Player1 + 0x6E, pad);
    c->a = pad;
    SetNZ16(pad);
    SetFlag(FC, fc);
}

static void FilterPlayer1Pad(void) { FilterPlayer1Pad_core(); SetM8(0); Ret_RTL(); }

/* C0:1602  Per-frame input: pads, multitap, CC:96BB (a scripted pad value
 * from $1C76 while $1000 is set), player 1 filter; players 2-4 in battle
 * mode ($B9 < 0: C0:167C/16D3/172A). */
static void InputUpdate(void)
{
    Cpu *c = g_cpu;
    uint8_t p = c->p;
    ReadJoypads_core();
    c->p = p;
    MultitapCheck_core();
    c->p = p;
    SetM8(0);
    c->a = ram16(0x001000);
    if (c->a) {
        uint16_t sc = ram16(0x001C76);
        c->a = sc;
        if (sc) {
            ram16_set(0xC3, sc & 0xFFF0);
            c->a = 0;
            ram16_set(0x001C76, 0);
        }
    }
    SetX8(0);
    FilterPlayer1Pad_core();
    SetM8(0);
    c->a = ram16(0xB9);
    SetNZ16(c->a);
    if (c->a & 0x8000) {
        CallLong(0xC0167C);
        CallLong(0xC016D3);
        CallLong(0xC0172A);
    }
    Ret_RTL();
}


/* --- exit --- */

/* C1:41A8  Player [$6A] on the open gate: if it may leave (not already
 * leaving or locked, C1:9BFB agrees, exit not blocked, all panels done),
 * starts the stage-clear sequence. */
static void PlayerOnExitTile(void)
{
    Cpu *c = g_cpu;
    c->y = 2;
    c->a = ind16(0x6A, 2);
    SetNZ16(c->a);
    if (c->a & 0xC000) { SetFlag(FZ, 0); Ret_RTL(); return; }
    SetFlag(FZ, 1);
    c->y = 0x52;
    c->a = ind16(0x6A, 0x52);
    SetNZ16(c->a);
    SetFlag(FZ, !(c->a & 1));
    if (c->a & 1) { Ret_RTL(); return; }
    c->a = 4;
    ram16_set(0x52, 4);
    SetNZ16(4);
    CallLong(0xC19BFB);
    if (!GetFlag(FC)) { Ret_RTL(); return; }
    c->a = ram16(Exit_Blocked);
    SetNZ16(c->a);
    if (c->a & 0x8000) { Ret_RTL(); return; }
    c->a = ram16(Exit_PanelState);
    SetNZ16(c->a);
    if (!(c->a & 0x8000)) { Ret_RTL(); return; }
    c->y = 2;
    c->a = ind16(0x6A, 2) | 0xC000;
    ind16_set(0x6A, 2, c->a);
    c->a = 0x100;
    ram16_set(0x7E5E5C, 0x100);
    SetNZ16(0x100);
    CallLong(0xC08D52);
    CallLong(0xC08D3A);
    CallLong(0xC163E8);
    c->a = ram16(0x7E8060);
    SetNZ16(c->a);
    if (!(c->a & 0x8000)) {
        ram16_set(0x7E8050, 0x0004);
        ram16_set(0x7E8052, 0x0801);
        ram16_set(0x7E806A, 0x0012);
        ram16_set(0x7E800E, (ram16(0x7E800E) | 0x3000) & 0x3FFF);
        c->a = 0x0FFF;
        ram16_set(0x7E800C, 0x0FFF);
        SetNZ16(0x0FFF);
    }
    ram16_set(0x7E5E20, 0x8000);
    uint16_t scene = ram16(0x1A) & 0x0F;
    load_cv();
    cmp16(scene, 6);
    c->a = scene;
    SetNZ16(scene - 6);
    if (fc) {
        ram8_set(0x70, 0x73);
        ram8_set(0x71, 0xA2);
        ram8_set(0x72, 0xC3);
        c->a = (scene & 0xFF00) | 0xC3;
        SetNZ8(0xC3);
        store_cv();
        CallLong(0xC3A385);
        SetM8(0);
    } else store_cv();
    c->a = 2;
    ram16_set(0x40, 2);
    ram16_set(0x42, 0);
    SetNZ16(2);
    CallLong(0xC3A4B5);
    c->a = 0xFFFF;
    ram16_set(0x7E5E38, 0xFFFF);
    ram16_set(0x7E5E3A, 0xFFFF);
    SetNZ16(0xFFFF);
    Ret_RTL();
}



/* C1:22F3  The single switch panel (state $7E5AAC, cell $7E5AAE, gfx index
 * $7E5AB0): stepping on it ($5E) flips it and redraws ($106C/$106E). */
static void SwitchPanelToggle_core(void)
{
    uint16_t st = ram16(0x7E5AAC);
    if (st & 0x8000) return;
    if (ram16(0x7E5AAE) != ram16(dp_PlayerCell)) return;
    uint16_t gfx = (st & 1) ? 0x106E : 0x106C;
    WriteLong16(TileGfxMap + ram16(0x7E5AB0), gfx);
    ram16_set(0x7E5AB2, gfx);
    ram16_set(0x7E5AAC, st ^ 1);
}

/* C0:E46C  The bomb's owner (player 1 + $100 * (+$06 & $7F) via C0:E651, or
 * $7E8500 for bit 11 bombs via C0:E667; pointer in $70) gets it back: +$F8
 * placed count down, +$F1 bombs left = +$E1 - +$F8 (types 3 and 6 at +$F5:
 * none, or one when nothing is placed). */
static void BombReturnToOwner(void)
{
    if (ind16(0x6A, 0) & 0x0800) {
        ram8_set(0x70, 0x00);
        ram8_set(0x71, 0x85);
    } else {
        ram8_set(0x70, 0x00);
        ram8_set(0x71, ind8(0x6A, 6) | 0x80);
    }
    ram8_set(0x72, 0x7E);
    uint8_t placed = ind8(0x70, 0xF8);
    if (placed) ind8_set(0x70, 0xF8, --placed);
    uint8_t type = ind8(0x70, 0xF5), left;
    if (type == 3) left = 0;
    else if (type == 6) left = placed ? 0 : 1;
    else left = ind8(0x70, 0xE1) - placed;
    ind8_set(0x70, 0xF1, left);
}

/* C0:E14F  A finished blast: C0:E4E3 clears the bomb (state 0, its gfx and
 * tile back from TileGfxMap2/CellItems), C0:E5FE/E505/E558/E5AB do the same
 * for each direction's flame cells (and the panels there toggle), then
 * C0:E303 clears its BlastMap cells. */
static void FlameRestoreDir(uint16_t list)
{
    uint32_t b = dp_ptr(0x6A);
    uint16_t steps = (uint8_t)(PeekLong8(b + 7) + 1);
    ram16_set(0x4E, steps);
    ram8_set(0x72, ram8(0x6C));
    uint16_t p = (uint16_t)(b + list);
    ram16_set(0x70, p);
    uint32_t bank = b & 0xFF0000;
    for (;;) {
        uint32_t e = bank | p;
        uint16_t gfx = PeekLong16(e);
        if (gfx == 0xFFFF) break;
        WriteLong16(TileGfxMap + gfx, ReadLong16(TileGfxMap2 + gfx));
        uint16_t cell = PeekLong16(e + 2);
        ram16_set(dp_PlayerCell, cell);
        WriteLong16(TileMap + cell, ReadLong16(CellItems + cell));
        SwitchPanelToggle_core();
        GoldPanelToggle_core();
        ram16_set(0x4E, --steps);
        if (!steps) break;
        p += 6;
        ram16_set(0x70, p);
    }
}

static void BlastUnpaintDir(uint16_t list, int up, int set5e)
{
    uint32_t b = ram16(0x82) | (uint32_t)ram8(0x84) << 16;
    uint16_t steps = (uint8_t)(PeekLong8(b + 7) + 1);
    ram16_set(0x4E, steps);
    ram8_set(0x72, ram8(0x84));
    uint16_t p = (uint16_t)(b + list);
    ram16_set(0x70, p);
    uint32_t bank = b & 0xFF0000;
    for (;;) {
        uint32_t e = bank | p;
        uint16_t cell = PeekLong16(e + 2);
        if (up && cell >= 0x2000) break;
        if (PeekLong16(e) == 0xFFFF) break;
        if (set5e) ram16_set(0x5E, cell);
        WriteLong16(BlastMap + cell, 0);
        ram16_set(0x4E, --steps);
        if (!steps) break;
        p += 6;
        ram16_set(0x70, p);
    }
}

static void FlameCleanup(void)
{
    uint32_t b = dp_ptr(0x6A);
    WriteLong16(b, 0);
    uint16_t gfx = PeekLong16(b + BOMB_POS);
    WriteLong16(TileGfxMap + gfx, ReadLong16(TileGfxMap2 + gfx));
    uint16_t cell = PeekLong16(b + BOMB_CELL);
    WriteLong16(TileMap + cell, ReadLong16(CellItems + cell));
    FlameRestoreDir(0x10);
    FlameRestoreDir(0x4C);
    FlameRestoreDir(0x88);
    FlameRestoreDir(0xC4);
    ram16_set(0x82, ram16(0x6A));
    ram8_set(0x84, ram8(0x6C));
    WriteLong16(BlastMap + PeekLong16(b + BOMB_CELL), 0);
    BlastUnpaintDir(0x10, 1, 0);
    BlastUnpaintDir(0x4C, 0, 1);
    BlastUnpaintDir(0x88, 0, 1);
    BlastUnpaintDir(0xC4, 0, 0);
}

/* --- bombs ---
 * Bomb objects ($7F2000 + $100 * n), [$6A] while updated: +$00 state (bit 14
 * exploding, 12 frozen?, 11 alt anim, 7/6/0 fuse control, 5 no tile, 0-4 anim
 * set), +$02 gfx index, +$04 fuse, +$07 range, +$08/+$09 anim timer/frame,
 * +$0A cell, +$0C attr. BombList ($7E2960): live bombs, $FFFF-terminated. */
#define BombList 0x7E2960

/* C0:E174 and C0:E195/E1A9/E203/E255/E2A7: mark the cells a blast reaches
 * in BlastMap: the bomb's cell, then each direction's entry list (written
 * by BlastExtend*) for range+1 entries, up to a solid/$43/$44 tile ($2000+
 * cells end the upward list too). [$82] = the bomb. */
static void BlastPaintDir(uint16_t list, int up, int set5e)
{
    uint32_t b = ram16(0x82) | (uint32_t)ram8(0x84) << 16;
    uint16_t steps = (uint8_t)(PeekLong8(b + 7) + 1);
    ram16_set(0x4E, steps);
    ram8_set(0x72, ram8(0x84));
    uint16_t p = (uint16_t)(b + list);
    ram16_set(0x70, p);
    uint32_t bank = b & 0xFF0000;
    for (;;) {
        uint32_t e = bank | p;
        uint16_t cell = PeekLong16(e + 2);
        if (up && cell >= 0x2000) break;
        if (PeekLong16(e) == 0xFFFF) break;
        if (set5e) ram16_set(0x5E, cell);   /* down/left keep the cell in $5E */
        g_cpu->x = cell;
        uint16_t tile = ReadLong16(TileMap + cell);
        if ((tile & 0x80) || tile == 0x44 || tile == 0x43) break;
        WriteLong16(BlastMap + cell, 0xFFFF);
        ram16_set(0x4E, --steps);
        if (!steps) break;
        p += 6;
        ram16_set(0x70, p);
    }
}

static void BlastPaint(void)
{
    ram16_set(0x82, ram16(0x6A));
    ram8_set(0x84, ram8(0x6C));
    uint32_t b = ram16(0x82) | (uint32_t)ram8(0x84) << 16;
    g_cpu->x = PeekLong16(b + BOMB_CELL);
    WriteLong16(BlastMap + g_cpu->x, 0xFFFF);
    BlastPaintDir(0x10, 1, 0);
    BlastPaintDir(0x4C, 0, 1);
    BlastPaintDir(0x88, 0, 1);
    BlastPaintDir(0xC4, 0, 0);
}


/* C0:EAA7/EB8E/EC68/ED42  When a bomb goes off, each direction's flame list
 * is checked cell by cell: what the flame hits (C1:46E9 objects; type $22 is
 * pushed via C1:2811, $16 via C1:26AA; tile $03 C0:EFD3, $02/$04/$0B
 * C0:EF44; solid, $26, $04, $0B tiles) cuts the list there ($FFFF). */
typedef struct BlastDirInfo {
    uint16_t list, opposite;
    uint16_t dir, delta;    /* $5E / $5C for C1:2811 */
    uint8_t up;
} BlastDirInfo;

static const BlastDirInfo kResolve[4] = {
    { 0x10, 0x4C, 3, 0xFF80, 1 },   /* up */
    { 0x4C, 0x10, 2, 0x0080, 0 },   /* down */
    { 0xC4, 0x88, 0, 0x0002, 0 },   /* right */
    { 0x88, 0xC4, 1, 0xFFFE, 0 },   /* left */
};

static void BlastResolveDir(const BlastDirInfo *d)
{
    Cpu *c = g_cpu;
    ram16_set(0x48, d->opposite);
    ram8_set(0x6F, ram8(0x6C));
    uint16_t steps = (ind8(0x6A, BOMB_RANGE) & 0xFF) + 1;
    ram16_set(0x4E, steps);
    uint16_t p = (uint16_t)(ram16(0x6A) + d->list);
    ram16_set(0x6D, p);
    uint32_t bank = (uint32_t)ram8(0x6C) << 16;
    for (;;) {
        uint32_t e = bank | p;
        int cut = 0;
        c->y = 2;
        if (d->up && PeekLong16(e + 2) >= 0x2000) cut = 1;
        else if (PeekLong16(e) == 0xFFFF) return;
        else {
            uint16_t cell = PeekLong16(e + 2);
            ram16_set(0x4C, cell);
            c->x = cell;
            uint16_t t = ReadLong16(TileMap + cell) & 0x3F;
            ram16_set(0x7E5BEE, t);
            int next = 0, done = 0;
            if (!(ram16(0xB9) & 0x8000)) {
                c->a = ram16(0xB9);
                CallLong(0xC146E9);
                if (GetFlag(FC)) cut = done = 1;
                else if (c->a == 0x22) {
                    ram16_set(0x5E, d->dir);
                    ram16_set(0x5C, d->delta);
                    c->a = d->delta;
                    CallLong(0xC12811);
                    cut = done = 1;
                } else if (c->a == 0x16) {
                    CallLong(0xC126AA);
                    cut = done = 1;
                }
            }
            if (!done) {
                t = ram16(0x7E5BEE);
                if (t == 0x0F || t == 0x09) next = 1;
                else if (t == 0x03) { c->a = t; CallLong(0xC0EFD3); cut = 1; }
                else {
                    int check = 1;
                    if (t == 0x02 || t == 0x04 || t == 0x0B) {
                        c->a = t;
                        CallLong(0xC0EF44);
                        if (ind16(0x6A, 0) & 4) {
                            check = 0;
                            if (ram16(0x7E5BEE) != 5) next = 1; else cut = 1;
                        }
                    }
                    if (check) {
                        uint16_t tile = ReadLong16(TileMap + ram16(0x4C));
                        uint16_t k = tile & 0x3F;
                        if ((tile & 0x80) || k == 0x26 || k == 0x04 || k == 0x0B) cut = 1;
                        else next = 1;
                    }
                }
            }
            (void)next;
        }
        if (cut) { WriteLong16(e, 0xFFFF); return; }
        ram16_set(0x4E, --steps);
        if (!steps) return;
        p += 6;
        ram16_set(0x6D, p);
    }
}

/* C0:EA83  The bomb starts exploding (state bit 14, anim reset) and its
 * flame lists are cut where they hit something. */
static void BombExplode(void)
{
    ind16_set(0x6A, 0, ind16(0x6A, 0) | 0x4000);
    ind16_set(0x6A, 8, 0);
    ram16_set(0x7E5C12, 0);
    for (int i = 0; i < 4; i++) BlastResolveDir(&kResolve[i]);
}

/* C0:E980  One live bomb: explodes when its cell holds a flame (tile 1, or
 * CellItems 1 outside $B9 < 0) or its fuse runs out (fuse ticks unless
 * state bit 0 without bit 7, or $B9 bit 14 without state bit 6); otherwise
 * keeps its tile ($43), animates (+$08/+$09 through the frame table at
 * [$7F], from C5:F4E7 per anim set or C5:F52B) and repaints its blast map. */
static void BombUpdate(void)
{
    Cpu *c = g_cpu;
    uint16_t st = ind16(0x6A, 0);
    if (st & 0x1000) return;
    uint16_t cell = ind16(0x6A, BOMB_CELL);
    c->x = cell;
    c->y = BOMB_CELL;
    int boom = 0;
    if (ReadLong16(TileMap + cell) == 1) { boom = 1; c->a = 1; }
    else if (!(ram16(0xB9) & 0x8000) && ReadLong16(CellItems + cell) == 1) { boom = 1; c->a = 1; }
    else {
        st = ind16(0x6A, 0);
        int tick;
        uint16_t a = st;
        if (!(st & 0x80) && (st & 1)) tick = 0;
        else if (st & 0x40) tick = 1;
        else { a = ram16(0xB9); tick = !(a & 0x4000); }
        if (tick) {
            uint8_t fuse = ind8(0x6A, 4) - 1;
            c->y = 4;
            if (!fuse) { boom = 1; c->a = a & 0xFF00; }
            else ind8_set(0x6A, 4, fuse);
        }
    }
    if (boom) {
        SetM8(0);
        BombReturnToOwner();
        BombExplode();
        c->a = 0x42;
        ram16_set(0x40, 0x42);
        ram16_set(0x42, 0);
        CallLong(0xC3A4B5);
        return;
    }
    SetM8(0);
    st = ind16(0x6A, 0);
    if (st & 0x20) return;
    cell = ind16(0x6A, BOMB_CELL);
    WriteLong16(TileMap + cell, 0x43);
    uint32_t tbl;
    if (st & 0x0800) tbl = 0xC5F52B;
    else tbl = PeekLong24(0xC5F4E7 + (st & 0x1F) * 4);
    ram8_set(0x7F, tbl);
    ram8_set(0x80, tbl >> 8);
    ram8_set(0x81, tbl >> 16);
    uint8_t t = ind8(0x6A, 8) - 1;
    ind8_set(0x6A, 8, t);
    if (t) return;
    ind8_set(0x6A, 8, 2);
    uint8_t f = ind8(0x6A, 9);
    ind8_set(0x6A, 9, f + 1);
    uint16_t code = PeekLong16(tbl + f * 2);
    if (code & 0x8000) {
        ind8_set(0x6A, 9, 0);
        code = PeekLong16(tbl);
    }
    code |= ind8(0x6A, 0x0C) << 8;
    ram16_set(0x4C, code);
    WriteLong16(TileGfxMap + ind16(0x6A, BOMB_POS), code);
    BlastPaint();
}


/* C0:E679, E86D/E6CC/E757/E7E2  Flame graphics for the bomb's cell and each
 * direction's entries: frame +$09 of the animation for the segment type
 * (+4 of an entry; tables at C5:F423), attr +$0C; TileMap gets 1 (flame).
 * Per entry C0:E8F8 (scene 5) and, unless $B9 < 0, C1:28CD run first. */
static uint16_t flame_gfx(uint16_t seg)
{
    uint16_t ptr = PeekLong16(0xC5F423 + (uint16_t)(seg << 1));
    ram16_set(0x6D, ptr);
    ram8_set(0x6F, 0xC5);
    uint16_t code = PeekLong16(0xC50000 + (uint16_t)(ptr + ram16(0x4E)));
    code |= ind8(0x6A, 0x0C) << 8;
    ram16_set(0x4C, code);
    return code;
}

static void FlameDrawCenter(void)
{
    ram16_set(0x4E, (uint8_t)(ind8(0x6A, 9) << 1));
    flame_gfx(0);
    WriteLong16(TileMap + ind16(0x6A, BOMB_CELL), 1);
    WriteLong16(TileGfxMap + ind16(0x6A, BOMB_POS), ram16(0x4C));
}

static void FlameDrawDir(uint16_t list, int up)
{
    Cpu *c = g_cpu;
    ram16_set(0x48, 0);
    ram8_set(0x72, ram8(0x6C));
    ram16_set(0x4E, (uint8_t)(ind8(0x6A, 9) << 1));
    uint16_t steps = (uint8_t)(ind8(0x6A, BOMB_RANGE) + 1);
    ram16_set(0x48, steps);
    uint16_t p = (uint16_t)(ram16(0x6A) + list);
    ram16_set(0x70, p);
    uint32_t bank = (uint32_t)ram8(0x6C) << 16;
    for (;;) {
        uint32_t e = bank | p;
        uint16_t gfx = PeekLong16(e);
        if (up ? gfx >= 0x2000 : gfx == 0xFFFF) return;
        uint16_t seg = PeekLong16(e + 4);
        c->a = flame_gfx(seg);
        c->x = (uint16_t)(seg << 1);
        c->y = 0x0C;
        CallLong(0xC0E8F8);
        if (!(ram16(0xB9) & 0x8000)) { c->a = ram16(0xB9); CallLong(0xC128CD); }
        e = bank | ram16(0x70);
        WriteLong16(TileMap + PeekLong16(e + 2), 1);
        WriteLong16(TileGfxMap + PeekLong16(e), ram16(0x4C));
        ram16_set(0x48, --steps);
        if (!steps) return;
        p = ram16(0x70) + 6;
        ram16_set(0x70, p);
    }
}

/* C0:E0F0  An exploding bomb: counts down +$08, then draws the flames
 * (FlameDraw*) and steps +$09 through the frame lengths
 * at C5:F415 ($FF = done: C0:E14F clears up). */
static void BlastTick(void)
{
    Cpu *c = g_cpu;
    uint8_t t = ind8(0x6A, 8);
    if (t && --t) { ind8_set(0x6A, 8, t); return; }
    c->a &= 0xFF00;
    c->y = 8;
    SetM8(0);
    FlameDrawCenter();
    FlameDrawDir(0x10, 1);
    FlameDrawDir(0x4C, 0);
    FlameDrawDir(0x88, 0);
    FlameDrawDir(0xC4, 0);
    SetM8(0);
    uint8_t f = ind8(0x6A, 9);
    c->x = f;
    uint8_t len = PeekLong8(0xC5F415 + f);
    if (len != 0xFF) {
        ind8_set(0x6A, 8, len);
        ind8_set(0x6A, 9, f + 1);
        return;
    }
    ind8_set(0x6A, 9, 0);
    FlameCleanup();
}


/* C0:DEEE (C0:DEE7 unless $7E5D60 < 0)  BombList = the live bombs (state
 * bit 15) of the 40 slots, in slot order. */
static void BombListRebuild(void)
{
    ram8_set(0x6A, 0x00);
    ram8_set(0x6B, 0x20);
    ram8_set(0x6C, 0x7F);
    ram16_set(BombList, 0xFFFF);
    uint16_t x = 0;
    for (int n = 0x28; n; n--) {
        ram16_set(0x1E, n);
        if (ind16(0x6A, 0) & 0x8000) {
            ram16_set(BombList + x, ram16(0x6A));
            x += 2;
            ram16_set(BombList + x, 0xFFFF);
        }
        ram16_set(0x6A, ram16(0x6A) + 0x100);
    }
    ram16_set(0x1E, 0);
    g_cpu->x = x;
    g_cpu->y = 0;
}

/* C0:DF30  With $7E5D60 < 0: rebuilds the list, then live bombs (not
 * exploding / frozen, fuse not about to fire) standing on CellItems $17-$1E
 * go to C0:DF87. */
static void BombsOnItems(void)
{
    Cpu *c = g_cpu;
    BombListRebuild();
    for (uint16_t i = 0;; i += 2) {
        ram16_set(0x1C, i);
        uint16_t b = ram16(BombList + i);
        if (b == 0xFFFF) break;
        ram16_set(0x6A, b);
        uint16_t st = ind16(0x6A, 0);
        if (st & 0x5000) continue;
        if (!((st & 1) && !(st & 0x80)) && ind8(0x6A, 4) == 1) continue;
        uint16_t cell = ind16(0x6A, BOMB_CELL);
        uint16_t t = ReadLong16(CellItems + cell) & 0x3F;
        if (t >= 0x17 && t < 0x1F) {
            c->a = t;
            c->x = cell;
            c->y = BOMB_CELL;
            CallLong(0xC0DF87);
        }
    }
}


/* C0:3583  Bomb range: the player's +$F2, or 9 on the listed cells
 * ($7E2FF0, 4-byte entries) in scene 6 / scene group $10. [$7C] = bomb. */
static void BombSetRange(void)
{
    uint16_t scene = ram16(0x1A);
    if (scene == 6 || (scene & 0xF0) == 0x10) {
        uint16_t cell = ind16(0x6A, OBJ_CELL);
        for (uint16_t x = 0;; x += 4) {
            uint16_t w = ram16(0x7E2FF0 + x);
            if (w & 0x8000) break;
            if (w == cell) { ind8_set(0x7C, BOMB_RANGE, 9); return; }
        }
    }
    ind8_set(0x7C, BOMB_RANGE, ind8(0x6A, 0xF2));
}

/* C0:34C0  Player [$6A] places a bomb in the first free slot ([$7C]): kind
 * +$F3 (0 when bit 1 is set and a bomb is already out), fuse $48, owner
 * (player index), attr, range, serial +$F6, its cell gets tile $43. Its one
 * caller does not look at registers or flags afterwards. */
static void BombPlace_core(void)
{
    ram8_set(0x7C, 0x00);
    ram8_set(0x7D, 0x20);
    ram8_set(0x7E, 0x7F);
    for (int n = 0x28;; ) {
        if (!(ind16(0x7C, 0) & 0x8000)) break;
        ram16_set(0x7C, ram16(0x7C) + 0x100);
        if (!--n) return;
    }
    uint8_t f3 = ind8(0x6A, 0xF3);
    uint16_t kind = ((f3 & 2) && ind8(0x6A, 0xF8)) ? 0 : f3;
    ind8_set(0x6A, 0x42, kind);
    ind16_set(0x7C, 0, kind | 0x8000);
    ind16_set(0x7C, BOMB_POS, ind16(0x6A, 0xAA));
    ind8_set(0x7C, 4, 0x48);
    uint8_t owner = ram8(0x6B) & 0x7F;
    ind8_set(0x7C, 6, owner);
    ind8_set(0x7C, 0x0C, (uint8_t)((owner + 1) << 2));
    BombSetRange();
    uint16_t serial = ind16(0x6A, 0xF6) + 1;
    ind16_set(0x6A, 0xF6, serial);
    ind8_set(0x7C, 5, serial);
    ind16_set(0x7C, 8, 1);
    uint16_t cell = ind16(0x6A, OBJ_CELL);
    ind16_set(0x7C, BOMB_CELL, cell);
    WriteLong16(TileMap + cell, 0x43);
    if (ind16(0x7C, 0) & 2) ind8_set(0x7C, BOMB_RANGE, 9);
}

static void BombPlace(void) { BombPlace_core(); SetM8(0); Ret_RTL(); }

/* C0:E0AA  Every other frame ($32 even): bomb list upkeep (C0:DF30 when
 * $7E5D60 < 0, C0:DEE7), then each bomb in BombList; stops after the first
 * one that starts exploding. Its callers (the stage loops) ignore its
 * registers and flags, so only WRAM and the register widths are kept. */
static void BombsUpdate(void)
{
    Cpu *c = g_cpu;
    if (ram16(0x32) & 1) { Ret_RTL(); return; }
    if (ram16(0x7E5D60) & 0x8000) BombsOnItems();
    else BombListRebuild();
    for (uint16_t i = 0;; i += 2) {
        ram16_set(0x1C, i);
        c->x = i;
        uint16_t bomb = ram16(BombList + i);
        if (bomb == 0xFFFF) break;
        ram16_set(0x6A, bomb);
        uint16_t st = ind16(0x6A, 0);
        c->a = st;
        if (st & 0x4000) { BlastTick(); SetM8(0); continue; }
        BombUpdate();
        SetM8(0);
        if (ind16(0x6A, 0) & 0x4000) break;
    }
    SetM8(0);
    Ret_RTL();
}

/* --- bomb button --- */

/* C0:31EB  A bomb already lies under player [$6A] (or its tile is $11). */
static int BombAtPlayer(void)
{
    uint16_t t = ind8(0x6A, 0xB4) & 0x3F;
    if (t == 0x11) return 1;
    if (t == 0x01) return 0;
    ram8_set(0x7C, 0x00); ram8_set(0x7D, 0x20); ram8_set(0x7E, 0x7F);
    for (uint16_t i = 0;; i += 2) {
        ram16_set(0x1E, i);
        uint16_t b = ram16(BombList + i);
        if (b == 0xFFFF) return 0;
        ram16_set(0x7C, b);
        if (!(ind16(0x7C, 0) & 0x5000) && ind16(0x7C, BOMB_POS) == ind16(0x6A, 0xAA)) return 1;
    }
}

/* C0:319A  An object ($7E2620 list, $8400+ only: type bit 8 or type $29)
 * stands on the player's cell, by $7E2720 or its own +$A8. */
static int ObjectOnPlayerCell(void)
{
    ram8_set(0x6F, 0x7E);
    for (uint16_t i = 0;; i += 2) {
        ram16_set(0x5C, i);
        uint16_t o = ram16(0x7E2620 + i);
        if (o == 0xFFFF) return 0;
        if (o < 0x8400) continue;
        ram16_set(0x6D, o);
        uint16_t t = ind16(0x6D, 0x7E);
        if (!(t & 0x100) && (t & 0x3F) != 0x29) continue;
        uint16_t cell = ind16(0x6A, OBJ_CELL);
        ram16_set(0x5E, cell);
        if (ram16(0x7E2720 + i) == cell || ind16(0x6D, OBJ_CELL) == cell) return 1;
    }
}

/* C0:3163  Nine bombs out already (not in battle mode). */
static int TooManyBombs(void)
{
    if (ram16(0xB9) & 0x8000) return 0;
    uint16_t n = 0;
    while (ram16(BombList + 2 * n) != 0xFFFF) n++;
    return n >= 9;
}

/* C0:3082  Player [$6A]'s bomb button (+$6E bit 7, edge via +$0E bit 10):
 * places a bomb if allowed and works out its blast (BlastExtend*, BlastMap
 * incl. C1:07CB's extra cells). Kick/remote/line-bomb cases stay in asm
 * (C0:2CAF, C0:3055, C0:2F6E). Its caller ignores registers. */
static void BombButton(void)
{
    int place = 0;
    if (!(ind16(0x6A, 0x6E) & 0x80)) {
        if (ind16(0x6A, 0x52) & 4) { CallLong(0xC02CAF); Ret_RTL(); return; }
        if (ind8(0x6A, 0xF5) == 2) {
            SetM8(0);
            CallLong(0xC03055);
            if (!GetFlag(FC)) { Ret_RTL(); return; }
            place = 1;
        } else {
            ind16_set(0x6A, 0x0E, ind16(0x6A, 0x0E) & 0xFBFF);
            SetM8(0); Ret_RTL(); return;
        }
    }
    if (!place) {
        uint16_t e = ind16(0x6A, 0x0E);
        if (e & 0x400) { SetM8(0); Ret_RTL(); return; }
        ind16_set(0x6A, 0x0E, e | 0x400);
        uint8_t b4 = ind8(0x6A, 0xB4);
        g_cpu->x = b4;
        if (b4 & 0x40) {
            if (ind8(0x6A, 0xFE) && (b4 & 0x3F) == 3 && ind16(0x6A, 0x50) != 8) {
                SetM8(0);
                CallLong(0xC02F6E);
            }
            SetM8(0); Ret_RTL(); return;
        }
    }
    SetM8(0);
    if (BombAtPlayer() || ObjectOnPlayerCell() || TooManyBombs()) { SetM8(0); Ret_RTL(); return; }
    uint8_t left = ind8(0x6A, 0xF1);
    if (!left) { SetM8(0); Ret_RTL(); return; }
    ind8_set(0x6A, 0xF1, left - 1);
    BombPlace_core();
    SetM8(0);
    ram16_set(0x40, 0x48);
    ram16_set(0x42, 0);
    g_cpu->a = 0x48;
    CallLong(0xC3A4B5);
    SetM8(0);
    ind8_set(0x6A, 0xF8, ind8(0x6A, 0xF8) + 1);
    ram16_set(BlastHitSpecial, 0);
    WriteLong16(BlastMap + ind16(0x7C, BOMB_CELL), 0xFFFF);   /* C0:3185 */
    BlastExtend_core(&kBlastUp);
    BlastExtend_core(&kBlastDown);
    BlastExtend_core(&kBlastLeft);
    BlastExtend_core(&kBlastRight);
    if (ram16(BlastHitSpecial)) {                              /* C1:07CB */
        uint16_t i = 0, c;
        while ((c = PeekLong16(0xC5E55C + i)) != 0xFFFF) {
            ram16_set(0x5A, i);
            WriteLong16(BlastMap + c, 0xFFFF);
            i += 2;
        }
        ram16_set(0x5A, i);
    }
    SetM8(0);
    Ret_RTL();
}


/* C1:4ED5  Enemy [$6A] whose timer +$0C reached $0FFF: dies (+$68 bit 3,
 * type $96, or types $90/$98 with +$BE < 0), or goes on to C1:4FFB (bit 2),
 * C1:4F6A (bit 1) or C1:4F25. Each case is a tail call. */
static void EnemyTimerDone(void)
{
    Cpu *c = g_cpu;
    c->y = 0x0C;
    c->a = ind16(0x6A, 0x0C);
    uint16_t d = cmp16(c->a, 0x0FFF);
    SetNZ16(d);
    SetFlag(FC, fc);
    if (c->a != 0x0FFF) { Ret_RTL(); return; }
    c->y = 0x68;
    uint16_t f = ind16(0x6A, 0x68);
    c->a = f;
    SetFlag(FN, f & 0x8000);
    if (f & 8) { SetFlag(FZ, 0); EnemyDie(); return; }
    if (f & 4) {
        c->y = 0x7E;
        uint16_t t = ind16(0x6A, 0x7E);
        c->a = t;
        if (t == 0x96) { SetNZ16(0); SetFlag(FC, 1); EnemyDie(); return; }
        if (t == 0x90 || t == 0x98) {
            SetFlag(FC, 1);
            c->y = 0xBE;
            c->a = ind16(0x6A, 0xBE);
            SetNZ16(c->a);
            if (c->a & 0x8000) { EnemyDie(); return; }
            CallLong(0xC14FFB);
            Ret_RTL();
            return;
        }
        SetNZ16(cmp16(t, 0x98));
        SetFlag(FC, fc);
        CallLong(0xC14FFB);
        Ret_RTL();
        return;
    }
    SetFlag(FZ, !(f & 2));
    CallLong(f & 2 ? 0xC14F6A : 0xC14F25);
    Ret_RTL();
}


/* --- frame drivers --- */

/* C0:FCA4  The normal-game stage loop, from the exit of its NMI wait (the
 * wait itself, C0:FC95-FCA2, stays in asm so frame boundaries are where
 * they were): one frame of game logic as a list of calls (ported routines
 * run natively through their hooks), then on to the next wait, or C0:FD42
 * when the stage ends. */
static const uint32_t kStageFrameCalls[] = {   /* jsl at C0:FCBD + 4 n */
    0xC00C36, 0xC1368C, 0xC00000, 0xC0D8E8, 0xC01602 /* InputUpdate */,
    0xC13CE7, 0xC152C5, 0xC10F10, 0xCCEAC4, 0xC0DCB4,
    0xC0E0AA /* BombsUpdate */, 0xC0DA42, 0xC00617, 0xC005EA, 0xC1334B,
    0xC1232C, 0xC11C1A /* StageTimer */, 0xC11BA3 /* ScoreDigits */,
    0xC10971, 0xC137EF, 0xC00CAA, 0xC15B3E,
};

static uint16_t Pull16(void) { uint16_t lo = Pull8(); return lo | Pull8() << 8; }

static void StageFrame(void)
{
    Cpu *c = g_cpu;
    static int st;
    static unsigned i;
    g_driver = StageFrame;
    PT_BEGIN(st);
    if (--c->x) { SetNZ16(c->x); JumpLong(0xC0FC9C); return; }   /* dex / bne */
    c->a = Pull16();
    c->x = Pull16();
    ram16_set(0x16, 0);                 /* LogicDone */
    c->a = 0;
    SetNZ16(0);
    for (i = 0; i < sizeof kStageFrameCalls / sizeof *kStageFrameCalls; i++)
        PT_CALL(st, kStageFrameCalls[i], 0xC0FCC1 + 4 * i);
    {
        uint16_t want = ram16(0x7E5D42), stage = ram16(0x1A);
        if (want != stage) {            /* stage change requested */
            SetFlag(FC, want >= stage);
            ram16_set(0x1A, want - 1);
            c->a = ram16(0x7E8002) | 0x4000;
            ram16_set(0x7E8002, c->a);
            SetNZ16(c->a);
            JumpLong(0xC0FD42);
            st = 0;
            return;
        }
    }
    PT_CALL(st, 0xC1363E, 0xC0FD31);
    ram16_set(0x16, ram16(0x16) + 1);
    c->a = ram16(0xB9);
    SetFlag(FN, c->a & 0x8000);
    if (c->a & 0x4000) { SetFlag(FZ, 0); JumpLong(0xC0FD42); st = 0; return; }
    PT_CALL(st, 0xC100BD, 0xC0FD3E);
    JumpLong(0xC0FC95);
    PT_END(st);
}


/* C0:FB20  The other stage loop (C0:FB11; used where the gold-panel/exit
 * logic of C0:FC95 is not), from the exit of its NMI wait: its jsl list,
 * then the next wait, or C0:FBA6 when the stage ends ($B9 bit 14 and $BE
 * clear). */
static const uint32_t kStageFrameBCalls[] = {  /* jsl at C0:FB39 + 4 n */
    0xC00C36, 0xC00000, 0xC01602 /* InputUpdate */, 0xC0D8E8, 0xC10C3C,
    0xC0D843, 0xC036E6, 0xC01E1D, 0xC10CAF, 0xC0DCB4,
    0xC0E0AA /* BombsUpdate */, 0xC0E076, 0xC0DA42, 0xC00617, 0xC005EA,
    0xC1334B, 0xC09615, 0xC11C1A /* StageTimer */, 0xC10971, 0xC00CAA,
    0xC00A38,
};

static void StageFrameB(void)
{
    Cpu *c = g_cpu;
    static int st;
    static unsigned i;
    g_driver = StageFrameB;
    PT_BEGIN(st);
    if (--c->x) { SetNZ16(c->x); JumpLong(0xC0FB18); return; }
    c->a = Pull16();
    c->x = Pull16();
    ram16_set(0x16, 0);
    c->a = 0;
    SetNZ16(0);
    for (i = 0; i < sizeof kStageFrameBCalls / sizeof *kStageFrameBCalls; i++)
        PT_CALL(st, kStageFrameBCalls[i], 0xC0FB3D + 4 * i);
    ram16_set(0x16, ram16(0x16) + 1);
    c->a = ram16(0xB9);
    if (c->a & 0x4000) {
        c->a = ram16(0xBE);
        SetNZ16(c->a);
        JumpLong(c->a ? 0xC0FB11 : 0xC0FBA6);
        st = 0;
        return;
    }
    SetFlag(FN, c->a & 0x8000);
    SetFlag(FZ, 1);
    PT_CALL(st, 0xC100BD, 0xC0FB9A);
    JumpLong(0xC0FB11);
    PT_END(st);
}

/* CC:0054  MainLoop (CC:003C) after its NMI wait: the scene/task driver for
 * everything outside the stage loop. Mode 7 registers from $7F400C-$7F4019,
 * frame counter $7F4006 (also $1C7A), input, then the task list at $7F0000
 * ($40 bytes each, +$00 entry point (24-bit), +$06 next index, $FFFF end;
 * the first entry is run before CC:7241), then CC:E7AF; $7F003C/$7F003E,
 * if set, is the next scene (jml), else back to the wait. */
static void SceneTaskSetup(uint16_t x)
{
    Cpu *c = g_cpu;
    c->x = x;
    ram16_set(0x6A, ram16(0x7F0000 + x));
    ram16_set(0x6B, ram16(0x7F0001 + x));
    c->a = ram16(0x7F0001 + x);
}

static void MainLoopFrame(void)
{
    Cpu *c = g_cpu;
    static int st;
    static uint16_t x;
    g_driver = MainLoopFrame;
    PT_BEGIN(st);
    if (--c->x) { SetNZ16(c->x); JumpLong(0xCC004C); return; }
    c->a = Pull16();
    c->x = Pull16();
    SetM8(0);
    c->a &= 0xFF00;
    WriteLong8(0x00420C, 0);
    PT_CALL(st, 0xCC7235, 0xCC0066);
    {
        static const struct { uint16_t reg, src; uint8_t two; } kMode7[] = {
            { 0x211B, 0x4012, 1 }, { 0x211C, 0x4014, 1 }, { 0x211D, 0x4016, 1 },
            { 0x211E, 0x4018, 1 }, { 0x211F, 0x400C, 1 }, { 0x2120, 0x400E, 1 },
            { 0x211A, 0x4010, 0 },
        };
        for (int k = 0; k < 7; k++) {
            WriteLong8(kMode7[k].reg, ram8(0x7F0000 | kMode7[k].src));
            if (kMode7[k].two) WriteLong8(kMode7[k].reg, ram8(0x7F0001 | kMode7[k].src));
        }
        SetM8(0);
        uint16_t f = ram16(0x7F4006) + 1;
        ram16_set(0x7F4006, f);
        ram16_set(0x001C7A, f);
        c->a = f;
        SetNZ16(f);
    }
    PT_CALL(st, 0xCC779A, 0xCC00E3);
    PT_CALL(st, 0xCC77CF, 0xCC00E7);
    SetM8(0);
    ram16_set(0x7F4000, 0x2200);
    ram8_set(0x7F4002, 0x7E);
    c->a = 0x227E;
    SetNZ8(0x7E);
    SetM8(1);
    PT_CALL(st, 0xCC7731, 0xCC00FC);
    PT_CALL(st, 0xC01602, 0xCC0100);              /* InputUpdate */
    PT_CALL(st, 0xCC75EB, 0xCC0104);
    SetM8(0);
    SceneTaskSetup(0);
    PT_CALL(st, 0xCC03E1, 0xCC0119);    /* jml [$006A] */
    PT_CALL(st, 0xCC7241, 0xCC011D);
    x = 0x40;
    for (;;) {
        SceneTaskSetup(x);
        PT_CALL(st, 0xCC03E1, 0xCC0132);
        SetM8(0);
        x = ram16(0x7F0006 + c->x);
        c->x = x;
        c->a = x + 1;
        SetNZ16(c->a);
        if (x == 0xFFFF) break;
    }
    PT_CALL(st, 0xCCE7AF, 0xCC0140);
    SetM8(0);
    c->a = ram16(0x7F003C);
    SetNZ16(c->a);
    if (!c->a) { JumpLong(0xCC003C); st = 0; return; }
    ram16_set(0x6A, c->a);
    c->a = ram16(0x7F003E);
    ram16_set(0x6C, c->a);
    SetNZ16(c->a);
    JumpLong(ram16(0x6A) | (uint32_t)ram8(0x6C) << 16);   /* jml [$006A] */
    PT_END(st);
}

/* C0:BEA6  Object [$6A]'s cell from its x/y (+$10/+$18): tile code -> +$B4
 * (soft block $C2 reads $40 with +$FB set; $41 while $1C78 is set), cell ->
 * +$A8 (the old one -> +$3A when it changes). Returns A = X = cell. */
static void ObjTileUpdate_core(void)
{
    Cpu *c = g_cpu;
    ram16_set(dp_TileY, ind16(0x6A, 0x18));
    ram16_set(dp_TileX, ind16(0x6A, 0x10));
    uint16_t tile = MapTileAtXY_core();
    if (tile == 0xC2 && ind8(0x6A, 0xFB)) tile = 0x40;
    ram16_set(dp_PlayerCell, tile);
    ind8_set(0x6A, 0xB4, tile);
    if (ram16(0x001C78)) ind8_set(0x6A, 0xB4, 0x41);
    uint16_t cell = c->x, old = ind16(0x6A, OBJ_CELL);
    cmp16(cell, old);
    if (cell != old) ind16_set(0x6A, 0x3A, old);
    ind16_set(0x6A, OBJ_CELL, cell);
    c->a = cell;
    c->y = OBJ_CELL;
    SetNZ16(cell);
    SetM8(0);
}

static void ObjTileUpdate(void) { load_cv(); ObjTileUpdate_core(); store_cv(); Ret_RTL(); }

/* C0:A5C9  Player [$6A] hit check (not while leaving/locked/flashing): on a
 * flame tile it loses a heart (+$EF; $80+ = endless) and flashes for $C0
 * frames, or dies (C0:A743 first when +$52 bit 1); a $B9 < 0 special case
 * goes to C1:29E0/2A3F. Its callers follow with other calls and ignore its
 * registers. */
static void PlayerHitCheck(void)
{
    if (ind16(0x6A, 2) & 0x8000) { Ret_RTL(); return; }
    if (ind16(0x6A, 0) & 0x0400) { Ret_RTL(); return; }
    if (ind16(0x6A, 2) & 0x0200) { Ret_RTL(); return; }
    load_cv();
    ObjTileUpdate_core();
    if (ind16(0x6A, 4) & 0x20) { Ret_RTL(); return; }
    ram16_set(0x7E5D5A, 0);
    uint16_t t = ind8(0x6A, 0xB4) & 0x3F;
    if (t != 1) {                       /* ($C8/$D0 cases can't match) */
        Ret_RTL();
        return;
    }
    if (!(ram16(0xB9) & 0x8000) && (ind16(0x6A, 0x60) & 0x8000)) {
        CallLong(0xC129E0);
        CallLong(0xC12A3F);
        Ret_RTL();
        return;
    }
    uint8_t hearts = ind8(0x6A, 0xEF);
    if (hearts) {
        if (hearts < 0x80) ind8_set(0x6A, 0xEF, hearts - 1);
        ind16_set(0x6A, 4, ind16(0x6A, 4) | 0x20);
        ind8_set(0x6A, 0xB9, 0xC0);
        SetM8(0);
        Ret_RTL();
        return;
    }
    if (ind16(0x6A, 0x52) & 2) {
        ram16_set(0x85, ram16(0x6A));
        ram8_set(0x87, ram8(0x6C));
        CallLong(0xC0A743);
        ram16_set(0x6A, ram16(0x85));
        ram8_set(0x6C, ram8(0x87));
    }
    SetM8(0);
    ind8_set(0x6A, 0xF5, 0);
    ind16_set(0x6A, 4, ind16(0x6A, 4) & 0xFFBF);
    ind16_set(0x6A, 0, ind16(0x6A, 0) | 0x0800);
    ind16_set(0x6A, 0x52, 0x0801);
    uint16_t kind = ram16(0x7E5D5A);
    uint16_t x = kind ? 7 : 0;
    ind16_set(0x6A, 0x50, x);
    ind16_set(0x6A, 0x56, x);
    ind16_set(0x6A, 0x6A, kind == 1 ? 0x1C : 0x1E);
    ind16_set(0x6A, 0x0E, (ind16(0x6A, 0x0E) | 0x3000) & 0x3FFF);
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
    { 0xC0A077, "SpritePieceOffset", SpritePieceOffset, FM | FX, 0, 0 },
    { 0xC09FFB, "SpritePriority",    SpritePriority,    FM | FX, 0, 0 },
    { 0xC0A02D, "SpritePieceAttr",   SpritePieceAttr,   FX,      0, 0 },
    { 0xC0A0B8, "SpriteHighBits",    SpriteHighBits,    FM | FX, 0, 0 },
    { 0xC14E1E, "DrawObjectSprite",  DrawObjectSprite,  FM | FX, 0, 0 },
    { 0xC0F117, "MergeCellMask",     MergeCellMask,     0,       0,  0 },
    { 0xC0A515, "TileGfxIndexAtXY",  TileGfxIndexAtXY,  FM | FX, 0, 0 },
    { 0xC037E2, "FindTileInMap",     FindTileInMap,     FM | FX, 0, 0 },
    { 0xC03778, "FindItemInMap",     FindItemInMap,     FM | FX, 0, 0 },
    { 0xC12558, "FindGoldPanels",    FindGoldPanels,    FM | FX, 0, 0 },
    { 0xC12A9C, "FindExitGate",      FindExitGate,      FM | FX, 0, 0 },
    { 0xC15028, "EnemyDie",          EnemyDie,          FM | FX, 0, 0 },
    { 0xC141A8, "PlayerOnExitTile",  PlayerOnExitTile,  FM | FX, 0, 0 },
    { 0xC0A570, "ObjFlashTick",      ObjFlashTick,      FM | FX, 0, 0 },
    { 0xC11E6F, "EnemyStun",         EnemyStun,         FM | FX, 0, 0 },
    { 0xC11EE2, "EnemyHit",          EnemyHit,          FX,      0, 0 },
    { 0xC0FCA4, "StageFrame",        StageFrame,        FM | FX, 0, NO_SHADOW },
    { 0xC0BEA6, "ObjTileUpdate",     ObjTileUpdate,     FM | FX, 0, 0 },
    { 0xC0A5C9, "PlayerHitCheck",    PlayerHitCheck,    FM | FX, 0,
      IGN_A | IGN_X | IGN_Y | IGN_NZ | IGN_C | IGN_V },
    { 0xC0FB20, "StageFrameB",       StageFrameB,       FM | FX, 0, NO_SHADOW },
    { 0xCC0054, "MainLoopFrame",     MainLoopFrame,     FM | FX, 0, NO_SHADOW },
    { 0xC14ED5, "EnemyTimerDone",    EnemyTimerDone,    FM | FX, 0, 0 },
    { 0xC03082, "BombButton",        BombButton,        FM | FX, 0,
      IGN_A | IGN_X | IGN_Y | IGN_NZ | IGN_C | IGN_V },
    { 0xC034C0, "BombPlace",         BombPlace,         FM | FX, 0,
      IGN_A | IGN_X | IGN_Y | IGN_NZ | IGN_C | IGN_V },
    { 0xC01797, "ReadJoypads",       ReadJoypads,       0,       0, 0 },
    { 0xC01862, "MultitapCheck",     MultitapCheck,     0,       0, 0 },
    { 0xC01625, "FilterPlayer1Pad",  FilterPlayer1Pad,  FX,      0, 0 },
    { 0xC01602, "InputUpdate",       InputUpdate,       0,       0, 0 },
    { 0xC11C1A, "StageTimer",        StageTimer,        FM | FX, 0,
      IGN_A | IGN_X | IGN_Y | IGN_NZ | IGN_C | IGN_V },
    { 0xC11BA3, "ScoreDigits",       ScoreDigits,       FM | FX, 0,
      IGN_A | IGN_X | IGN_Y | IGN_NZ | IGN_C | IGN_V },
    { 0xC0E0AA, "BombsUpdate",       BombsUpdate,       FM | FX, 0,
      IGN_A | IGN_X | IGN_Y | IGN_NZ | IGN_C | IGN_V },
};
const int g_nhooks = sizeof g_hooks / sizeof *g_hooks;
