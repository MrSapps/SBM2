"""65816 opcode table.

Each entry is (mnemonic, mode). Operand size per mode is in MODE_SIZE; the
two immediate modes whose size depends on the M/X flags are IMM_M and IMM_X.
"""

IMP, ACC = 'imp', 'acc'
IMM_M, IMM_X, IMM8 = 'imm_m', 'imm_x', 'imm8'
DP, DPX, DPY = 'dp', 'dpx', 'dpy'
DPIND, DPINDX, DPINDY = 'dpind', 'dpindx', 'dpindy'
DPINDL, DPINDLY = 'dpindl', 'dpindly'
SR, SRINDY = 'sr', 'srindy'
ABS, ABSX, ABSY = 'abs', 'absx', 'absy'
LONG, LONGX = 'long', 'longx'
ABSIND, ABSINDX, ABSINDL = 'absind', 'absindx', 'absindl'
REL, RELL = 'rel', 'rell'
MOVE = 'move'

MODE_SIZE = {
    IMP: 0, ACC: 0, IMM8: 1, DP: 1, DPX: 1, DPY: 1, DPIND: 1, DPINDX: 1,
    DPINDY: 1, DPINDL: 1, DPINDLY: 1, SR: 1, SRINDY: 1, REL: 1,
    ABS: 2, ABSX: 2, ABSY: 2, ABSIND: 2, ABSINDX: 2, ABSINDL: 2, RELL: 2,
    MOVE: 2, LONG: 3, LONGX: 3,
}

OPS = [None] * 256

# The eight ALU-style ops share one column layout.
_ALU = ['ora', 'and', 'eor', 'adc', 'sta', 'lda', 'cmp', 'sbc']
_ALU_COLS = {
    0x01: DPINDX, 0x03: SR, 0x05: DP, 0x07: DPINDL, 0x09: IMM_M, 0x0D: ABS,
    0x0F: LONG, 0x11: DPINDY, 0x12: DPIND, 0x13: SRINDY, 0x15: DPX,
    0x17: DPINDLY, 0x19: ABSY, 0x1D: ABSX, 0x1F: LONGX,
}
for _i, _m in enumerate(_ALU):
    for _col, _mode in _ALU_COLS.items():
        OPS[(_i << 5) + _col] = (_m, _mode)
OPS[0x89] = ('bit', IMM_M)  # there is no "sta #imm"

_REST = {
    0x00: ('brk', IMM8), 0x02: ('cop', IMM8), 0x04: ('tsb', DP), 0x06: ('asl', DP),
    0x08: ('php', IMP), 0x0A: ('asl', ACC), 0x0B: ('phd', IMP), 0x0C: ('tsb', ABS),
    0x0E: ('asl', ABS),
    0x10: ('bpl', REL), 0x14: ('trb', DP), 0x16: ('asl', DPX), 0x18: ('clc', IMP),
    0x1A: ('inc', ACC), 0x1B: ('tcs', IMP), 0x1C: ('trb', ABS), 0x1E: ('asl', ABSX),
    0x20: ('jsr', ABS), 0x22: ('jsl', LONG), 0x24: ('bit', DP), 0x26: ('rol', DP),
    0x28: ('plp', IMP), 0x2A: ('rol', ACC), 0x2B: ('pld', IMP), 0x2C: ('bit', ABS),
    0x2E: ('rol', ABS),
    0x30: ('bmi', REL), 0x34: ('bit', DPX), 0x36: ('rol', DPX), 0x38: ('sec', IMP),
    0x3A: ('dec', ACC), 0x3B: ('tsc', IMP), 0x3C: ('bit', ABSX), 0x3E: ('rol', ABSX),
    0x40: ('rti', IMP), 0x42: ('wdm', IMM8), 0x44: ('mvp', MOVE), 0x46: ('lsr', DP),
    0x48: ('pha', IMP), 0x4A: ('lsr', ACC), 0x4B: ('phk', IMP), 0x4C: ('jmp', ABS),
    0x4E: ('lsr', ABS),
    0x50: ('bvc', REL), 0x54: ('mvn', MOVE), 0x56: ('lsr', DPX), 0x58: ('cli', IMP),
    0x5A: ('phy', IMP), 0x5B: ('tcd', IMP), 0x5C: ('jml', LONG), 0x5E: ('lsr', ABSX),
    0x60: ('rts', IMP), 0x62: ('per', RELL), 0x64: ('stz', DP), 0x66: ('ror', DP),
    0x68: ('pla', IMP), 0x6A: ('ror', ACC), 0x6B: ('rtl', IMP), 0x6C: ('jmp', ABSIND),
    0x6E: ('ror', ABS),
    0x70: ('bvs', REL), 0x74: ('stz', DPX), 0x76: ('ror', DPX), 0x78: ('sei', IMP),
    0x7A: ('ply', IMP), 0x7B: ('tdc', IMP), 0x7C: ('jmp', ABSINDX), 0x7E: ('ror', ABSX),
    0x80: ('bra', REL), 0x82: ('brl', RELL), 0x84: ('sty', DP), 0x86: ('stx', DP),
    0x88: ('dey', IMP), 0x8A: ('txa', IMP), 0x8B: ('phb', IMP), 0x8C: ('sty', ABS),
    0x8E: ('stx', ABS),
    0x90: ('bcc', REL), 0x94: ('sty', DPX), 0x96: ('stx', DPY), 0x98: ('tya', IMP),
    0x9A: ('txs', IMP), 0x9B: ('txy', IMP), 0x9C: ('stz', ABS), 0x9E: ('stz', ABSX),
    0xA0: ('ldy', IMM_X), 0xA2: ('ldx', IMM_X), 0xA4: ('ldy', DP), 0xA6: ('ldx', DP),
    0xA8: ('tay', IMP), 0xAA: ('tax', IMP), 0xAB: ('plb', IMP), 0xAC: ('ldy', ABS),
    0xAE: ('ldx', ABS),
    0xB0: ('bcs', REL), 0xB4: ('ldy', DPX), 0xB6: ('ldx', DPY), 0xB8: ('clv', IMP),
    0xBA: ('tsx', IMP), 0xBB: ('tyx', IMP), 0xBC: ('ldy', ABSX), 0xBE: ('ldx', ABSY),
    0xC0: ('cpy', IMM_X), 0xC2: ('rep', IMM8), 0xC4: ('cpy', DP), 0xC6: ('dec', DP),
    0xC8: ('iny', IMP), 0xCA: ('dex', IMP), 0xCB: ('wai', IMP), 0xCC: ('cpy', ABS),
    0xCE: ('dec', ABS),
    0xD0: ('bne', REL), 0xD4: ('pei', DP), 0xD6: ('dec', DPX), 0xD8: ('cld', IMP),
    0xDA: ('phx', IMP), 0xDB: ('stp', IMP), 0xDC: ('jml', ABSINDL), 0xDE: ('dec', ABSX),
    0xE0: ('cpx', IMM_X), 0xE2: ('sep', IMM8), 0xE4: ('cpx', DP), 0xE6: ('inc', DP),
    0xE8: ('inx', IMP), 0xEA: ('nop', IMP), 0xEB: ('xba', IMP), 0xEC: ('cpx', ABS),
    0xEE: ('inc', ABS),
    0xF0: ('beq', REL), 0xF4: ('pea', ABS), 0xF6: ('inc', DPX), 0xF8: ('sed', IMP),
    0xFA: ('plx', IMP), 0xFB: ('xce', IMP), 0xFC: ('jsr', ABSINDX), 0xFE: ('inc', ABSX),
}
for _op, _e in _REST.items():
    assert OPS[_op] is None, hex(_op)
    OPS[_op] = _e
assert all(OPS), [hex(i) for i, e in enumerate(OPS) if e is None]

# Control-flow classes
BRANCHES = {'bpl', 'bmi', 'bvc', 'bvs', 'bcc', 'bcs', 'bne', 'beq'}
ENDS_BLOCK = {'rts', 'rtl', 'rti', 'stp', 'bra', 'brl', 'jmp', 'jml'}


def operand_size(op, m8, x8):
    mode = OPS[op][1]
    if mode == IMM_M:
        return 1 if m8 else 2
    if mode == IMM_X:
        return 1 if x8 else 2
    return MODE_SIZE[mode]
