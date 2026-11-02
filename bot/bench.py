"""Benchmark the bot on its own (no assists) over a library of stage starts.

  bench.py collect               copy build/*/stage_XX.mss (+ .inputs) into
                                 build/lib/ (first one found per stage)
  bench.py run <out> [-f FRAMES] [-j JOBS] [stage ...]
                                 run each stage start for FRAMES frames
                                 (default 15000, a full 4-minute stage) with SBM2_NOASSIST=1,
                                 then print a table; stages are hex ids (21)
  bench.py report <out>          the table again

A stage counts as cleared when the bot log shows a later STAGE line; frames
is when that happened. Deaths include ones a rollback undid (search effort).
"""
import argparse
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, 'build', 'lib')
DRIVE = os.path.join(ROOT, 'bot', 'drive.py')


def collect():
    os.makedirs(LIB, exist_ok=True)
    found = 0
    for path in sorted(glob.glob(os.path.join(ROOT, 'build', '*', 'stage_??.mss'))):
        name = os.path.basename(path)
        side = path[:-4] + '.inputs'
        dest = os.path.join(LIB, name)
        if os.path.exists(dest) or not os.path.exists(side):
            continue
        shutil.copy(path, dest)
        shutil.copy(side, dest[:-4] + '.inputs')
        found += 1
    print(f'{found} new; library: {" ".join(sorted(s[6:8] for s in os.listdir(LIB) if s.endswith(".mss")))}')


def run_one(out, stage, frames):
    d = os.path.join(out, stage)
    shutil.rmtree(d, ignore_errors=True)
    env = dict(os.environ, SBM2_NOASSIST='1')
    subprocess.run([sys.executable, DRIVE, d, os.path.join(LIB, f'stage_{stage}.mss'), str(frames)],
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return stage


def result(out, stage):
    d = os.path.join(out, stage)
    stages, deaths, rollbacks, end = [], 0, 0, '-'
    for log in sorted(glob.glob(os.path.join(d, 'bot_*.log'))):
        for line in open(log, errors='replace'):
            m = re.match(r'\s*(\d+) STAGE ([0-9A-F]{2})', line)
            if m:
                stages.append((int(m.group(1)), m.group(2)))
            elif ' END ' in line:
                end = ' '.join(line.split()[2:])
            elif 'DEATH' in line:
                deaths += 1
            elif 'rollback to' in line:
                rollbacks += 1
    clear = next((f for f, s in stages if s != stage), None)
    return clear, deaths, rollbacks, end


def report(out):
    rows = []
    for stage in sorted(os.listdir(out)):
        if os.path.isdir(os.path.join(out, stage)):
            rows.append((stage,) + result(out, stage))
    print(f'{"stage":6} {"cleared":>8} {"deaths":>7} {"rollbk":>7}  end (stage score/soft/panels enemies)')
    for stage, clear, deaths, rb, end in rows:
        print(f'{stage[0]}-{int(stage[1], 16) + 1:<4} {clear if clear is not None else "-":>8} {deaths:>7} {rb:>7}  {end}')
    n = sum(1 for r in rows if r[1] is not None)
    times = sorted(r[1] for r in rows if r[1] is not None)
    med = times[len(times) // 2] if times else '-'
    print(f'cleared {n}/{len(rows)}, median frames {med}, deaths {sum(r[2] for r in rows)}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['collect', 'run', 'report'])
    ap.add_argument('out', nargs='?')
    ap.add_argument('stages', nargs='*')
    ap.add_argument('-f', '--frames', type=int, default=15000)
    ap.add_argument('-j', '--jobs', type=int, default=3)
    a = ap.parse_args()
    if a.cmd == 'collect':
        return collect()
    out = os.path.abspath(a.out)
    if a.cmd == 'run':
        stages = a.stages or sorted(s[6:8] for s in os.listdir(LIB) if s.endswith('.mss'))
        os.makedirs(out, exist_ok=True)
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            for s in ex.map(lambda s: run_one(out, s, a.frames), stages):
                print(s, *result(out, s), flush=True)
    report(out)


if __name__ == '__main__':
    main()
