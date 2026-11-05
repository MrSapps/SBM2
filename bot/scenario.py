"""Scenario tests: the bot from a fixed moment of a stage, on the port
(build/port/botrun), with a pass condition on its log. Seconds each.

A scenario (bot/test/scenarios/NAME.txt, text) is a stage start state
(build/lib or build/libp stage_XX.mss) plus the pads that take the game from
there to the moment under test, run-length encoded per logic frame:

    # what this checks
    start 34              stage start state
    budget 3000           frames the bot gets from the moment on
    pass STAGE 35         regex the bot log must match (search)
    fail rollback         optional: regex that fails it at once
    seed 0                optional (SBM2_SEED)
    env SBM2_X=1          optional, repeatable
    pads                  then "<repeat> <word>,<word>..." lines: one logic
    120 0001,0000         frame each (the $0001 NMI marker and its pad reads,
    ...                   bot/inputlog.lua format), repeated <repeat> times

Usage:
  scenario.py make NAME RUN_DIR FRAME [--start XX] [--budget N] --pass RE [--note TEXT]
      cut a scenario from a bench run (RUN_DIR/run.inputs; FRAME = the
      bot's frame in that run, rollbacks already cut out of run.inputs)
  scenario.py run [NAME...] [-j N] [-v]
      run scenarios (all by default); exit status 1 if any fails
"""
import argparse
import concurrent.futures
import glob
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, 'bot', 'test', 'scenarios')
BOTRUN = os.path.join(ROOT, 'build', 'port', 'botrun')
LIB, LIBP = os.path.join(ROOT, 'build', 'lib'), os.path.join(ROOT, 'build', 'libp')


def words(path):
    data = open(path, 'rb').read()
    return list(struct.unpack(f'<{len(data) // 2}H', data[:len(data) // 2 * 2]))


def start_state(stage):
    p = os.path.join(LIBP, f'stage_{stage}.mss')
    return p if os.path.exists(p) else os.path.join(LIB, f'stage_{stage}.mss')


def encode(ws):
    """Groups of words per logic frame (each starts at a $0001 marker), RLE."""
    groups, cur = [], []
    for w in ws:
        if w == 1 and cur:
            groups.append(tuple(cur)); cur = []
        cur.append(w)
    if cur:
        groups.append(tuple(cur))
    lines, i = [], 0
    while i < len(groups):
        j = i
        while j < len(groups) and groups[j] == groups[i]:
            j += 1
        lines.append(f'{j - i} ' + ','.join(f'{w:04X}' for w in groups[i]))
        i = j
    return lines


def parse(path):
    sc = {'name': os.path.basename(path)[:-4], 'env': {}, 'seed': '0', 'fail': None, 'pads': []}
    in_pads = False
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if in_pads:
            n, ws = line.split()
            sc['pads'] += [int(w, 16) for w in ws.split(',')] * int(n)
            continue
        k, _, v = line.partition(' ')
        if k == 'pads':
            in_pads = True
        elif k == 'env':
            a, _, b = v.partition('=')
            sc['env'][a] = b
        elif k in ('start', 'budget', 'pass', 'fail', 'seed'):
            sc[k] = v
        else:
            raise SystemExit(f'{path}: unknown key {k}')
    return sc


def make(a):
    run = os.path.abspath(a.run)
    log = words(os.path.join(run, 'run.inputs'))
    stage = a.start
    if not stage:
        sts = sorted(glob.glob(os.path.join(run, 'stage_*.mss')))
        stage = os.path.basename(sts[0])[6:8]
    side = words(start_state(stage)[:-4] + '.inputs')
    if log[:len(side)] != side:
        raise SystemExit(f'{run}/run.inputs does not start with stage {stage}\'s pad log')
    rest, n, cut = log[len(side):], 0, None
    for i, w in enumerate(rest):
        if w == 1:
            if n == a.frame:
                cut = i; break
            n += 1
    if cut is None:
        raise SystemExit(f'the run has only {n} logic frames after stage {stage}\'s start')
    out = os.path.join(DIR, a.name + '.txt')
    os.makedirs(DIR, exist_ok=True)
    with open(out, 'w') as f:
        if a.note:
            for ln in a.note.splitlines():
                f.write(f'# {ln}\n')
        f.write(f'# cut from {os.path.relpath(run, ROOT)} at frame {a.frame}\n')
        f.write(f'start {stage}\nbudget {a.budget}\npass {a.pass_}\n')
        if a.fail:
            f.write(f'fail {a.fail}\n')
        f.write('pads\n' + '\n'.join(encode(rest[:cut])) + '\n')
    print(f'{out}: {a.frame} frames of pads, {os.path.getsize(out)} bytes')


def run_one(sc, verbose):
    out = tempfile.mkdtemp(prefix=f'scen_{sc["name"]}_', dir=os.path.join(ROOT, 'build'))
    pre = os.path.join(out, 'prefix.bin')
    with open(pre, 'wb') as f:
        f.write(struct.pack(f'<{len(sc["pads"])}H', *sc['pads']))
    env = dict(os.environ, SBM2_OUT=out, SBM2_FRAMES=sc['budget'], SBM2_LOAD=start_state(sc['start']),
               SBM2_PREFIX=pre, SBM2_NOASSIST='1', SBM2_SEED=sc['seed'], SBM2_CKPT='1000000', **sc['env'])
    t0 = time.time()
    subprocess.run([BOTRUN, os.path.join(ROOT, 'baserom.sfc'), os.path.join(ROOT, 'bot', 'bot.lua')],
                   env=env, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, 'stderr.txt'), 'w'))
    dt = time.time() - t0
    text = ''.join(open(p, errors='replace').read() for p in sorted(glob.glob(os.path.join(out, 'bot_*.log'))))
    ok, why = False, 'no match'
    pm = re.search(sc['pass'], text)
    fm = re.search(sc['fail'], text) if sc['fail'] else None
    if fm and (not pm or fm.start() < pm.start()):
        why = 'fail: ' + text[text.rfind('\n', 0, fm.start()) + 1:text.find('\n', fm.start())].strip()
    elif pm:
        ok, why = True, text[text.rfind('\n', 0, pm.start()) + 1:text.find('\n', pm.start())].strip()
    return sc['name'], ok, why, dt, out


def run(a):
    paths = [os.path.join(DIR, n + '.txt') for n in a.names] if a.names else sorted(glob.glob(os.path.join(DIR, '*.txt')))
    scs = [parse(p) for p in paths]
    for sc in scs:
        if a.budget:
            sc['budget'] = a.budget
    fails = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for name, ok, why, dt, out in ex.map(lambda s: run_one(s, a.verbose), scs):
            fails += not ok
            print(f'{"ok  " if ok else "FAIL"} {name:28} {dt:5.1f}s  {why}' + ('' if ok else f'  [{os.path.relpath(out, ROOT)}]'), flush=True)
    print(f'{len(scs) - fails} passed, {fails} failed')
    sys.exit(1 if fails else 0)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make')
    m.add_argument('name'); m.add_argument('run'); m.add_argument('frame', type=int)
    m.add_argument('--start'); m.add_argument('--budget', type=int, default=3000)
    m.add_argument('--pass', dest='pass_', required=True); m.add_argument('--fail')
    m.add_argument('--note')
    r = sub.add_parser('run')
    r.add_argument('names', nargs='*'); r.add_argument('-j', '--jobs', type=int, default=8)
    r.add_argument('-v', '--verbose', action='store_true')
    r.add_argument('-b', '--budget', help='override every scenario\'s budget (frames)')
    a = ap.parse_args()
    make(a) if a.cmd == 'make' else run(a)


if __name__ == '__main__':
    main()
