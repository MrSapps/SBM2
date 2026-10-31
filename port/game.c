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
 * $4E = dy (word), A = y, Y = $84, flags from the add. */
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
    SetNZ16(c->a);
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
};
const int g_nhooks = sizeof g_hooks / sizeof *g_hooks;
