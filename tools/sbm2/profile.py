"""Sum a port profile (build/port/hybrid -P FILE: "pc count" lines) by
routine. Routine starts are the sub_XXXXXX labels of the src/ listing plus
the code names in symbols.txt; hooked routines (port/game.c) are marked.

Usage: profile.py PROFILE [N]"""
import bisect, glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
starts = {}
for path in glob.glob(os.path.join(ROOT, 'src', 'bank_*.asm')):
    for m in re.finditer(r'^sub_([0-9A-F]{6}):', open(path).read(), re.M):
        starts[int(m[1], 16)] = 'sub_' + m[1]
for line in open(os.path.join(ROOT, 'tools', 'sbm2', 'symbols.txt')):
    m = re.match(r'([0-9A-F]{2}):([0-9A-F]{4})\s+(\S+)', line)
    if m:
        starts[int(m[1] + m[2], 16)] = m[3]
hooked = {int(a, 16) for a in re.findall(r'\{ 0x([0-9A-F]{6}),',
                                         open(os.path.join(ROOT, 'port', 'game.c')).read())}
addrs = sorted(starts)
tot, total = {}, 0
for line in open(sys.argv[1]):
    pc, n = line.split()
    pc, n = int(pc, 16), int(n)
    total += n
    i = bisect.bisect_right(addrs, pc) - 1
    a = addrs[i] if i >= 0 and addrs[i] >> 16 == pc >> 16 else pc & 0xFFFF00
    tot[a] = tot.get(a, 0) + n
print(f'interpreted instructions: {total}')
for a, n in sorted(tot.items(), key=lambda x: -x[1])[:int(sys.argv[2]) if len(sys.argv) > 2 else 30]:
    print(f'{a:06X} {starts.get(a, "?"):24s} {n:12d} {100 * n / total:5.1f}%{"  hooked" if a in hooked else ""}')
