"""A video of the C++ agent playing, with overlays of what it thinks.

Runs a scenario with the agent writing its overlay file (SBM2_OVERLAY),
then replays the run (exact) through build/port/play, which draws the game
at 4x with the overlays (port/overlay.c) and encodes 1920x1080 60 fps.

Usage:
  video.py SCENARIO OUT.mp4 [--seed N] [--from F] [--to F] [--speed K] [--watch]
    SCENARIO   e.g. agent_world1 (bot/test/scenarios/NAME.txt; one with
               no pads, from a stage start)
    --from/--to  video frames of the run to keep (default: all)
    --speed K  keep every Kth frame
    --watch    show it in a window instead of writing OUT.mp4
"""
import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'bot'))
import scenario  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scenario')
    ap.add_argument('out')
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--from', dest='vfrom', type=int, default=0)
    ap.add_argument('--to', dest='vto', type=int, default=-1)
    ap.add_argument('--speed', type=int, default=1)
    ap.add_argument('--watch', action='store_true')
    a = ap.parse_args()
    sc = scenario.parse(os.path.join(scenario.DIR, a.scenario + '.txt'))
    if sc['pads']:
        raise SystemExit('a scenario from a stage start (no pads) only')
    sc['seed'] = str(a.seed)
    out = os.path.abspath(a.out)
    ov = os.path.splitext(out)[0] + '.ov'
    for t in ('build/port/agentrun', 'build/port/play'):
        subprocess.run(['make', '-s', t], cwd=ROOT, check=True)
    os.environ['SBM2_OVERLAY'] = ov
    print(f'agent run: {a.scenario} seed {a.seed} ...', flush=True)
    _, ok, why, dt, d = scenario.run_one(sc, False)
    print(f'  {"ok" if ok else "FAIL"} {why} ({dt:.0f}s), {d}', flush=True)
    start = scenario.start_state(sc['start'])[:-4] + '.inputs'
    cmd = [os.path.join(ROOT, 'build/port/play'), os.path.join(ROOT, 'baserom.sfc'),
           '-r', os.path.join(d, 'run.inputs'), '-t', start, '-O', ov]
    if a.watch:
        cmd += ['-f', '60']
    else:
        cmd += ['-V', out, '-F', str(a.vfrom), '-T', str(a.vto), '-k', str(a.speed)]
    print('render: ' + ' '.join(cmd), flush=True)
    subprocess.run(['nice', '-n', '19'] + cmd, check=True)
    if not a.watch:
        print(f'wrote {out}')


if __name__ == '__main__':
    main()
