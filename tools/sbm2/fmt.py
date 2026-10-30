"""Format decoded instructions as asar-syntax assembly."""
from opcodes import OPS, REL, RELL

_FMT = {
    'imp': '', 'acc': 'A', 'imm8': '#${:02X}', 'dp': '${:02X}', 'dpx': '${:02X},x',
    'dpy': '${:02X},y', 'dpind': '(${:02X})', 'dpindx': '(${:02X},x)',
    'dpindy': '(${:02X}),y', 'dpindl': '[${:02X}]', 'dpindly': '[${:02X}],y',
    'sr': '${:02X},s', 'srindy': '(${:02X},s),y', 'abs': '${:04X}',
    'absx': '${:04X},x', 'absy': '${:04X},y', 'long': '${:06X}',
    'longx': '${:06X},x', 'absind': '(${:04X})', 'absindx': '(${:04X},x)',
    'absindl': '[${:04X}]',
}
# Explicit width suffixes so the assembler can never pick a different size.
_SUFFIX = {1: '.b', 2: '.w', 3: '.l'}


def branch_target(ins):
    mode = OPS[ins.op][1]
    nxt = (ins.addr + ins.size) & 0xFFFF
    v = ins.operand
    if mode == REL:
        v -= 0x100 if v >= 0x80 else 0
    else:
        v -= 0x10000 if v >= 0x8000 else 0
    return (ins.addr & 0xFF0000) | (nxt + v) & 0xFFFF


def format_insn(ins, label_for=None):
    """label_for(addr24) -> label name or None."""
    mn, mode = OPS[ins.op]
    if mode in (REL, RELL):
        tgt = branch_target(ins)
        name = label_for(tgt) if label_for else None
        return f'{mn} {name or f"${tgt & 0xFFFF:04X}"}'
    if mode == 'move':
        # asar 1.91 emits the operands in written order, so write them in
        # machine order (dest bank, then source bank).
        dst, src = ins.operand & 0xFF, ins.operand >> 8
        return f'{mn} ${dst:02X},${src:02X} ; dest=${dst:02X} src=${src:02X}'
    if mode in ('imm_m', 'imm_x'):
        n = ins.size - 1
        return f'{mn}.{"b" if n == 1 else "w"} #${ins.operand:0{2 * n}X}'
    if mode in ('imp', 'acc'):
        return mn + (' A' if mode == 'acc' else '')
    n = ins.size - 1
    sfx = _SUFFIX[n] if mode not in ('imm8', 'absind', 'absindx', 'absindl') else ''
    if mn in ('jsr', 'jmp', 'jsl', 'jml') and mode in ('abs', 'long') and label_for:
        tgt = ins.operand if mode == 'long' else (ins.addr & 0xFF0000) | ins.operand
        name = label_for(tgt)
        if name:
            return f'{mn} {name}'
    return f'{mn}{sfx} ' + _FMT[mode].format(ins.operand)
