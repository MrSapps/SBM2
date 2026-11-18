"""A video of the C++ agent playing, with overlays of what it thinks.

Runs a scenario with the agent writing its overlay file (SBM2_OVERLAY),
then replays the run (exact) through build/port/play, which draws the game
at 4x with the overlays (port/overlay.c) and encodes 1920x1080 60 fps.

Usage:
  video.py SCENARIO OUT.mp4 [--seed N] [--from F] [--to F] [--speed K] [--watch]
           [--tail N] [--split] [-j J] [--ov FILE --run DIR]
    SCENARIO   e.g. agent_world1 (bot/test/scenarios/NAME.txt; one with
               no pads, from a stage start)
    --from/--to  video frames of the run to keep (default: all; with
               --split, --to ends the last part)
    --speed K  keep every Kth frame
    --watch    show it in a window instead of writing OUT.mp4
    --tail N   after the scenario's last stage, N frames more with no input
               (SBM2_TAIL: e.g. the ending after agent_game)
    --split    one file per world (OUT_world1.mp4 .. and OUT_ending.mp4 for
               stage $60), rendered J at a time from the one recording,
               then joined losslessly (no re-encode) into OUT.mp4
    --ov/--run reuse an earlier run's overlay file and run dir (no agent run)
"""
import argparse
import os
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
    ap.add_argument('--tail', type=int, default=0)
    ap.add_argument('--split', action='store_true')
    ap.add_argument('-j', type=int, default=2)
    ap.add_argument('--ov')
    ap.add_argument('--run')
    a = ap.parse_args()
    sc = scenario.parse(os.path.join(scenario.DIR, a.scenario + '.txt'))
    if sc['pads']:
        raise SystemExit('a scenario from a stage start (no pads) only')
    sc['seed'] = str(a.seed)
    out = os.path.abspath(a.out)
    ov = os.path.splitext(out)[0] + '.ov'
    for t in ('build/port/agentrun', 'build/port/play'):
        subprocess.run(['make', '-s', t], cwd=ROOT, check=True)
    if a.run:
        ov, d = os.path.abspath(a.ov), os.path.abspath(a.run)
    else:
        os.environ['SBM2_OVERLAY'] = ov
        if a.tail:
            os.environ['SBM2_TAIL'] = str(a.tail)
        print(f'agent run: {a.scenario} seed {a.seed} ...', flush=True)
        _, ok, why, dt, d = scenario.run_one(sc, False)
        print(f'  {"ok" if ok else "FAIL"} {why} ({dt:.0f}s), {d}', flush=True)
    start = scenario.start_state(sc['start'])[:-4] + '.inputs'
    cmd = [os.path.join(ROOT, 'build/port/play'), os.path.join(ROOT, 'baserom.sfc'),
           '-r', os.path.join(d, 'run.inputs'), '-t', start, '-O', ov]
    if a.watch:
        cmd += ['-f', '60']
        subprocess.run(['nice', '-n', '19'] + cmd, check=True)
        return
    if not a.split:
        cmd += ['-V', out, '-F', str(a.vfrom), '-T', str(a.vto), '-k', str(a.speed)]
        print('render: ' + ' '.join(cmd), flush=True)
        subprocess.run(['nice', '-n', '19'] + cmd, check=True)
        print(f'wrote {out}')
        return
    parts = []
    for name, f in segments(ov):
        if parts:
            parts[-1][2] = f - 1
        parts.append([name, f, -1])
    parts[-1][2] = a.vto
    base = os.path.splitext(out)[0]
    jobs, procs = [], []
    for name, f, t in parts:
        o = f'{base}_{name}.mp4'
        jobs.append(o)
        c = cmd + ['-V', o, '-F', str(f), '-T', str(t), '-k', str(a.speed)]
        while len(procs) >= a.j:
            procs.pop(0).wait()
        print(f'render {name}: frames {f}..{t if t >= 0 else "end"} -> {o}', flush=True)
        procs.append(subprocess.Popen(['nice', '-n', '19'] + c))
    for p in procs:
        p.wait()
    lst = base + '_parts.txt'
    with open(lst, 'w') as fh:
        fh.writelines(f"file '{o}'\n" for o in jobs)
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'concat', '-safe', '0',
                    '-i', lst, '-c', 'copy', out], check=True)
    print(f'wrote {out} and ' + ', '.join(os.path.basename(o) for o in jobs))


def segments(ov):
    """(name, first video frame) per world, and 'ending' for stage $60,
    from the overlay file's G (stage start) lines."""
    out, last = [('world1', 0)], 1
    for line in open(ov):
        if not line.startswith('G '):
            continue
        _, f, st = line.split()
        w = int(st, 16) >> 4
        if w != last and w >= 2:
            out.append(('ending' if w == 6 else f'world{w}', int(f)))
            last = w
    return out

if __name__ == '__main__':
    main()
