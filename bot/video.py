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
               then joined into OUT.mp4: the video losslessly (no
               re-encode), the sound from the parts' raw samples (one
               encode: no gaps at the joins)
    --mute     no sound (it comes from play's shadow of the sound CPU,
               port/spc.h; with --speed > 1 there is none)
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
    ap.add_argument('--mute', action='store_true')
    a = ap.parse_args()
    sound = not a.mute and a.speed == 1
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
        v = out[:-4] + '.video.mp4' if sound else out
        cmd += ['-V', v, '-F', str(a.vfrom), '-T', str(a.vto), '-k', str(a.speed)]
        if sound:
            cmd += ['-A', out[:-4] + '.pcm']
        print('render: ' + ' '.join(cmd), flush=True)
        subprocess.run(['nice', '-n', '19'] + cmd, check=True)
        if sound:
            mux([v], [out[:-4] + '.pcm'], out)
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
        o = f'{base}_{name}.video.mp4' if sound else f'{base}_{name}.mp4'
        jobs.append(o)
        c = cmd + ['-V', o, '-F', str(f), '-T', str(t), '-k', str(a.speed)]
        if sound:
            c += ['-A', f'{base}_{name}.pcm']
        while len(procs) >= a.j:
            procs.pop(0).wait()
        print(f'render {name}: frames {f}..{t if t >= 0 else "end"} -> {o}', flush=True)
        procs.append(subprocess.Popen(['nice', '-n', '19'] + c))
    for p in procs:
        if p.wait():
            raise SystemExit('a part failed')
    if not sound:
        concat(jobs, out)
        print(f'wrote {out} and ' + ', '.join(os.path.basename(o) for o in jobs))
        return
    pcms = [f'{base}_{name}.pcm' for name, _, _ in parts]
    finals = []
    for (name, _, _), v, pcm in zip(parts, jobs, pcms):
        finals.append(f'{base}_{name}.mp4')
        mux([v], [pcm], finals[-1])
    mux(jobs, pcms, out)
    print(f'wrote {out} and ' + ', '.join(os.path.basename(o) for o in finals))


def concat(videos, out):
    """Join video files without re-encoding."""
    lst = out[:-4] + '_parts.txt'
    with open(lst, 'w') as fh:
        fh.writelines(f"file '{v}'\n" for v in videos)
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'concat', '-safe', '0',
                    '-i', lst, '-c', 'copy', out], check=True)


def mux(videos, pcms, out):
    """The videos joined (copied) with the parts' raw sound (play -A: 48 kHz,
    800 samples a frame, so it lines up), encoded once."""
    v = videos[0]
    if len(videos) > 1:
        v = out[:-4] + '.joined.mp4'
        concat(videos, v)
    pcm = pcms[0]
    if len(pcms) > 1:
        pcm = out[:-4] + '.all.pcm'
        with open(pcm, 'wb') as fo:
            for p in pcms:
                with open(p, 'rb') as fi:
                    while chunk := fi.read(1 << 24):
                        fo.write(chunk)
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', v, '-f', 's16le', '-ar', '48000', '-ac', '2',
                    '-i', pcm, '-map', '0:v', '-map', '1:a', '-c:v', 'copy', '-c:a', 'aac', '-b:a', '192k',
                    '-shortest', out], check=True)


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
