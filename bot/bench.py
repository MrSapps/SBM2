"""Benchmark the bot on its own (no assists) over a library of stage starts.

  bench.py collect               copy build/*/stage_XX.mss (+ .inputs) into
                                 build/lib/ (first one found per stage)
  bench.py run <out> [stage ...] [-f FRAMES] [-j JOBS] [-s SEEDS]
                                 run each stage start for FRAMES frames
                                 (default 15000, a full 4-minute stage) with SBM2_NOASSIST=1,
                                 then print a table; stages are hex ids (21);
                                 -s runs each stage with seeds 0..SEEDS-1
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
LIBP = os.path.join(ROOT, 'build', 'libp')
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


def run_one(out, stage, frames, seed=0):
    d = os.path.join(out, stage if seed == 0 else f'{stage}s{seed}')
    shutil.rmtree(d, ignore_errors=True)
    env = dict(os.environ, SBM2_NOASSIST='1', SBM2_SEED=str(seed))
    start = os.path.join(LIB, f'stage_{stage}.mss')
    # The port has its own start states where a Mesen pad log does not
    # replay in frame-step (build/libp/stage_XX.mss, preferred if present).
    if os.environ.get('SBM2_PORT') and os.path.exists(os.path.join(LIBP, f'stage_{stage}.mss')):
        start = os.path.join(LIBP, f'stage_{stage}.mss')
    subprocess.run([sys.executable, DRIVE, d, start, str(frames)],
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.basename(d)


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
            elif ' DEATH ' in line and 'contact:C0' not in line:
                # (only in the benched stage, not the ones a run goes on to;
                # contact:C0 was the exit warp in logs before the EXIT label)
                if not any(int(s, 16) > int(stage[:2], 16) for _, s in stages):
                    deaths += 1
            elif 'rollback to' in line:
                rollbacks += 1
    # A game over sends the bot back a stage: only a later stage is a clear.
    clear = next((f for f, s in stages if int(s, 16) > int(stage[:2], 16)), None)
    return clear, deaths, rollbacks, end


def report(out):
    # Runs of one stage with other seeds (bench.py -s) live in <stage>s<seed>.
    runs = {}
    for name in sorted(os.listdir(out)):
        if os.path.isdir(os.path.join(out, name)) and not name.startswith('_'):
            runs.setdefault(name[:2], []).append(result(out, name))
    print(f'{"stage":6} {"cleared":>8} {"frames":>7} {"deaths":>7}  end of the first run (stage score/soft/panels enemies)')
    total = cleared = deaths = 0
    times = []
    for stage, rs in sorted(runs.items()):
        ok = [r[0] for r in rs if r[0] is not None]
        med = sorted(ok)[len(ok) // 2] if ok else '-'
        d = sum(r[1] for r in rs)
        print(f'{stage[0]}-{int(stage[1], 16) + 1:<4} {len(ok):>4}/{len(rs):<3} {med:>7} {d:>7}  {rs[0][3]}')
        total, cleared, deaths = total + len(rs), cleared + len(ok), deaths + d
        times += ok
    times.sort()
    med = times[len(times) // 2] if times else '-'
    print(f'cleared {cleared}/{total} runs over {len(runs)} stages, median frames {med}, deaths {deaths}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['collect', 'run', 'report'])
    ap.add_argument('out', nargs='?')
    ap.add_argument('stages', nargs='*')
    ap.add_argument('-f', '--frames', type=int, default=15000)
    ap.add_argument('-j', '--jobs', type=int, default=3)
    ap.add_argument('-s', '--seeds', type=int, default=1)
    ap.add_argument('-p', '--port', action='store_true',
                    help='run the bot on the port (build/port/botrun), not Mesen')
    a = ap.parse_args()
    if a.cmd == 'collect':
        return collect()
    out = os.path.abspath(a.out)
    if a.port:
        os.environ['SBM2_PORT'] = '1'
    if a.cmd == 'run':
        # $60 is the ending, not a stage.
        stages = a.stages or sorted(s[6:8] for s in os.listdir(LIB) if s.endswith('.mss') and s[6:8] != '60')
        jobs = [(s, seed) for seed in range(a.seeds) for s in stages]
        os.makedirs(out, exist_ok=True)
        # Run a copy of bot/: edits made while the bench runs don't leak in.
        snap = os.path.join(out, '_bot')
        shutil.rmtree(snap, ignore_errors=True)
        shutil.copytree(os.path.join(ROOT, 'bot'), snap, ignore=shutil.ignore_patterns('test', '__pycache__'))
        os.environ['SBM2_BOT'] = os.path.join(snap, 'bot.lua')
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            for s in ex.map(lambda j: run_one(out, j[0], a.frames, j[1]), jobs):
                print(s, *result(out, s), flush=True)
    report(out)


if __name__ == '__main__':
    main()
