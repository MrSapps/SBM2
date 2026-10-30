"""Static control-flow tracer for the SBM2 ROM (HiROM, 8 Mbit).

Follows code from a set of entry points, tracking the M/X register widths,
and records every decoded instruction. Anything it can't follow statically
(indirect jumps, PLP with an unknown saved state, width conflicts) is reported
so it can be resolved by hand-written hints or a runtime coverage log.

Hints file format (one per line, '#' comments):
    code   C0FA00 m8 x8     # seed: code at this address with these widths
    noret  C08123           # this subroutine never returns to its caller
"""
import os
import sys
from collections import namedtuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from opcodes import OPS, BRANCHES, ENDS_BLOCK, operand_size, \
    ABS, LONG, REL, RELL, ABSIND, ABSINDX, ABSINDL

# Mesen 2 SNES CDL flag bits (one byte per PRG-ROM byte).
CDL_CODE, CDL_DATA, CDL_JUMP, CDL_SUB, CDL_X8, CDL_M8 = 1, 2, 4, 8, 0x10, 0x20

Insn = namedtuple('Insn', 'addr op size m8 x8 operand')


class Rom:
    def __init__(self, path):
        self.data = open(path, 'rb').read()

    def offset(self, addr):
        """Map a 24-bit SNES address to a file offset, or None if not ROM."""
        bank, lo = addr >> 16, addr & 0xFFFF
        if bank in (0x7E, 0x7F):
            return None
        if (bank & 0x7F) >= 0x40 or lo >= 0x8000:
            off = ((bank & 0x3F) << 16) | lo
            return off if off < len(self.data) else None
        return None

    def byte(self, addr):
        return self.data[self.offset(addr)]

    def word(self, addr):
        off = self.offset(addr)
        return self.data[off] | self.data[off + 1] << 8


def canon(addr):
    """Canonical name for a ROM address: its $C0-$FF bank mirror."""
    return ((addr >> 16 & 0x3F) | 0xC0) << 16 | (addr & 0xFFFF)


class Tracer:
    def __init__(self, rom):
        self.rom = rom
        self.insns = {}          # file offset -> Insn
        self.owner = {}          # file offset -> offset of insn covering it
        self.labels = {}         # canonical addr -> kind ('sub', 'loc')
        self.problems = []       # (addr, message)
        self.indirect = []       # (addr, insn text) unresolved jumps
        self.noret = set()
        self.cdl = None          # Mesen code/data log, if loaded

    def cdl_widths(self, off):
        """(m8, x8) recorded at runtime for an executed opcode, else None."""
        if self.cdl and self.cdl[off] & CDL_CODE:
            f = self.cdl[off]
            return bool(f & CDL_M8), bool(f & CDL_X8)
        return None

    def note(self, addr, msg):
        self.problems.append((addr, msg))

    def run(self, entries):
        work = list(entries)
        seen = set()
        while work:
            state = work.pop()
            if state in seen:
                continue
            seen.add(state)
            work.extend(self._block(*state))

    def _block(self, pc, m8, x8, php):
        """Decode one linear run; return successor states."""
        out = []
        while True:
            off = self.rom.offset(pc)
            if off is None:
                self.note(pc, 'execution leaves ROM')
                return out
            # Runtime-observed widths beat static inference.
            m8, x8 = self.cdl_widths(off) or (m8, x8)
            prev = self.insns.get(off)
            if prev:
                if (prev.m8, prev.x8) != (m8, x8):
                    self.note(pc, f'width conflict: m8={prev.m8},x8={prev.x8} '
                                  f'vs m8={m8},x8={x8}')
                return out
            if off in self.owner:
                self.note(pc, 'jumps into the middle of an instruction')
                return out

            op = self.rom.data[off]
            mn, mode = OPS[op]
            n = operand_size(op, m8, x8)
            raw = self.rom.data[off + 1:off + 1 + n]
            operand = int.from_bytes(raw, 'little') if n else None
            ins = Insn(pc, op, 1 + n, m8, x8, operand)
            for i in range(1 + n):
                if off + i in self.owner or (i and off + i in self.insns):
                    self.note(pc, 'overlaps another instruction')
                self.owner[off + i] = off
            self.insns[off] = ins
            bank = pc & 0xFF0000
            nxt = bank | (pc + 1 + n) & 0xFFFF

            if mn == 'rep':
                m8 = m8 and not operand & 0x20
                x8 = x8 and not operand & 0x10
            elif mn == 'sep':
                m8 = m8 or bool(operand & 0x20)
                x8 = x8 or bool(operand & 0x10)
            elif mn == 'php':
                php = php + ((m8, x8),)
            elif mn == 'plp':
                if php:
                    (m8, x8), php = php[-1], php[:-1]
                else:
                    self.note(pc, 'plp with unknown saved flags; keeping widths')

            if mn in BRANCHES or mn == 'bra' or mn == 'brl':
                disp = operand - (0x100 if mode == REL and operand >= 0x80 else 0)
                if mode == RELL and operand >= 0x8000:
                    disp = operand - 0x10000
                tgt = bank | (nxt + disp) & 0xFFFF
                self.labels.setdefault(canon(tgt), 'loc')
                out.append((tgt, m8, x8, php))
            elif mn in ('jsr', 'jmp') and mode == ABS:
                tgt = bank | operand
                self.labels[canon(tgt)] = 'sub' if mn == 'jsr' else \
                    self.labels.get(canon(tgt), 'loc')
                out.append((tgt, m8, x8, () if mn == 'jsr' else php))
                if mn == 'jsr' and canon(tgt) in self.noret:
                    return out
            elif mn in ('jsl', 'jml') and mode == LONG:
                self.labels[canon(operand)] = 'sub' if mn == 'jsl' else \
                    self.labels.get(canon(operand), 'loc')
                out.append((operand, m8, x8, () if mn == 'jsl' else php))
                if mn == 'jsl' and canon(operand) in self.noret:
                    return out
            elif mode in (ABSIND, ABSINDX, ABSINDL):
                self.indirect.append((pc, f'{mn} {mode} ${operand:04X}'))

            if mn in ENDS_BLOCK:
                return out
            pc = nxt


def load_hints(path):
    entries, noret = [], set()
    for line in open(path):
        line = line.split('#')[0].split()
        if not line:
            continue
        if line[0] == 'code':
            entries.append((int(line[1], 16), line[2] == 'm8', line[3] == 'x8', ()))
        elif line[0] == 'noret':
            noret.add(canon(int(line[1], 16)))
    return entries, noret


def cdl_entries(rom, cdl):
    """Seed states from a CDL: every executed byte that starts a run of
    code, then each following instruction by linear sweep through the run."""
    out = []
    off = 0
    while off < len(cdl):
        f = cdl[off]
        if not f & CDL_CODE:
            off += 1
            continue
        m8, x8 = bool(f & CDL_M8), bool(f & CDL_X8)
        out.append((0xC00000 | off, m8, x8, ()))
        off += 1 + operand_size(rom.data[off], m8, x8)
    return out


def vector_entries(rom):
    # Native-mode vectors at $FFE4.., emulation-mode at $FFF4..
    names = {0xFFE4: 'cop', 0xFFE6: 'brk', 0xFFEA: 'nmi', 0xFFEE: 'irq',
             0xFFFC: 'reset'}
    out = []
    for vec, name in names.items():
        tgt = rom.word(vec)
        # Interrupt handlers inherit unknown widths; Hudson code sets them
        # explicitly early, so assume 8-bit until proven otherwise.
        out.append((name, tgt, (tgt, True, True, ())))
    return out


if __name__ == '__main__':
    rom = Rom(sys.argv[1])
    t = Tracer(rom)
    entries = [e for _, _, e in vector_entries(rom)]
    if len(sys.argv) > 2:
        extra, t.noret = load_hints(sys.argv[2])
        entries += extra
    if len(sys.argv) > 3:
        t.cdl = open(sys.argv[3], 'rb').read()
        entries += cdl_entries(rom, t.cdl)
    t.run(entries)
    code = sum(i.size for i in t.insns.values())
    print(f'{len(t.insns)} insns, {code} bytes '
          f'({100 * code / len(rom.data):.2f}% of ROM), '
          f'{sum(1 for k in t.labels.values() if k == "sub")} subroutines')
    print(f'{len(t.indirect)} indirect jumps, {len(t.problems)} problems')
    for a, s in t.indirect:
        print(f'  indirect {canon(a):06X}: {s}')
    for a, s in t.problems:
        print(f'  problem  {canon(a):06X}: {s}')
