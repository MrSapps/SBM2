"""Emit the whole ROM as rebuildable asm, one file per 64 KiB bank.

Traced code becomes instructions; everything else is emitted as db bytes.
Usage: disasm.py baserom.sfc hints.txt outdir [coverage.cdl ...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace import Rom, Tracer, vector_entries, load_hints, cdl_entries, canon
from fmt import format_insn

DB_PER_LINE = 16


def label_name(addr, kind):
    return f'{kind}_{addr:06X}'


def emit(rom, t, outdir):
    starts = set(t.insns)
    # An instruction is emitted only if no other instruction starts inside it.
    good = {o for o, i in t.insns.items()
            if not any(o + k in starts for k in range(1, i.size))}
    named = {}
    for a, kind in t.labels.items():
        off = rom.offset(a)
        if off in good:
            named[a] = label_name(a, kind)

    # Branches and jsr/jmp only encode the low 16 bits, so any mirror works.
    def label_for(addr):
        return named.get(canon(addr))

    # Long refs must keep their exact bank byte, so only the canonical mirror.
    def long_label_for(addr):
        return named.get(addr) if canon(addr) == addr else None

    os.makedirs(outdir, exist_ok=True)
    banks = len(rom.data) // 0x10000
    with open(os.path.join(outdir, 'main.asm'), 'w') as f:
        f.write('hirom\n\n')
        for b in range(banks):
            f.write(f'incsrc "bank_{0xC0 + b:02X}.asm"\n')

    for b in range(banks):
        base = b * 0x10000
        lines = [f'org ${0xC0 + b:02X}0000', '']
        off = base
        pending = []

        def flush():
            if pending:
                lines.append('  db ' + ','.join(f'${x:02X}' for x in pending))
                pending.clear()

        while off < base + 0x10000:
            addr = 0xC00000 | off
            if addr in named:
                flush()
                lines.append(f'{named[addr]}:')
            if off in good:
                flush()
                ins = t.insns[off]
                mn = format_insn(ins, None).split()[0]
                lf = long_label_for if mn in ('jsl', 'jml') else label_for
                lines.append(f'  {format_insn(ins, lf)}')
                off += ins.size
                continue
            pending.append(rom.data[off])
            if len(pending) == DB_PER_LINE:
                flush()
            off += 1
        flush()
        with open(os.path.join(outdir, f'bank_{0xC0 + b:02X}.asm'), 'w') as f:
            f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    rom = Rom(sys.argv[1])
    t = Tracer(rom)
    entries = [e for *_, e in vector_entries(rom)]
    extra, t.noret = load_hints(sys.argv[2])
    for path in sys.argv[4:]:
        # Merge several CDLs (e.g. from different bot runs) by OR-ing flags.
        cdl = open(path, 'rb').read()
        t.cdl = cdl if t.cdl is None else bytes(a | b for a, b in zip(t.cdl, cdl))
    if t.cdl:
        extra += cdl_entries(rom, t.cdl)
    t.run(entries + extra)
    emit(rom, t, sys.argv[3])
    code = sum(i.size for i in t.insns.values())
    print(f'{code} code bytes ({100 * code / len(rom.data):.2f}%), '
          f'{len(t.problems)} problems')
