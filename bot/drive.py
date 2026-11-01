"""Run bot.lua in Mesen until a frame budget is reached, resuming from the
last checkpoint whenever Mesen exits early. Merges segment CDLs into run.cdl.
The pad log (bot/inputlog.lua) follows the state each segment starts from
(its .inputs sidecar: ckpt.inputs, or the world state's after a game over), so
<outdir>/run.inputs always replays the run from power-on (port/hybrid).

Usage: drive.py <outdir> <start.mss> <frames> [shots,...]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MESEN = os.path.join(ROOT, 'tools', 'mesen2')


def merge_cdls(out):
    merged = None
    for name in sorted(os.listdir(out)):
        if name.startswith('seg_') and name.endswith('.cdl'):
            data = open(os.path.join(out, name), 'rb').read()
            merged = bytearray(data) if merged is None else \
                bytearray(a | b for a, b in zip(merged, data))
    if merged is not None:
        open(os.path.join(out, 'run.cdl'), 'wb').write(merged)


def check_sidecar(state):
    if not os.path.exists(state[:-4] + '.inputs'):
        print(f'warning: {state} has no .inputs sidecar; no replayable log '
              '(bot/world_states.py makes them)', flush=True)


def main():
    out = os.path.abspath(sys.argv[1])
    start = os.path.abspath(sys.argv[2])
    frames = int(sys.argv[3])
    shots = sys.argv[4] if len(sys.argv) > 4 else ''
    os.makedirs(out, exist_ok=True)
    ckpt_txt = os.path.join(out, 'ckpt.txt')
    frame0, state = 0, start
    for attempt in range(1000):
        check_sidecar(state)
        env = dict(os.environ, SBM2_OUT=out, SBM2_FRAMES=str(frames),
                   SBM2_FRAME0=str(frame0), SBM2_LOAD=state, SBM2_SHOTS=shots)
        rc = subprocess.run(
            ['./Mesen', '--testrunner', '--timeout=86400', os.path.join(ROOT, 'baserom.sfc'),
             os.path.join(ROOT, 'bot', 'bot.lua')],
            cwd=MESEN, env=env, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL).returncode
        reached = int(open(ckpt_txt).read()) if os.path.exists(ckpt_txt) else 0
        print(f'segment from {frame0}: exit {rc}, checkpoint at {reached}', flush=True)
        if reached >= frames:
            break
        if reached <= frame0:
            # No progress since the last start: give up rather than loop.
            if rc != 0 and attempt > 0 and reached == frame0:
                print('no progress; stopping', flush=True)
                break
        frame0, state = reached, os.path.join(out, 'ckpt.mss')
        over = os.path.join(out, 'gameover.txt')
        if os.path.exists(over):
            # Continue: restart the stage (or else the world) we were on.
            world = int(open(over).read())
            os.remove(over)
            state = os.path.join(ROOT, 'build', 'states', f'world{world}.mss')
            # Prefer the start of the stage we were on (bot.lua saves it).
            if os.path.exists(os.path.join(out, 'stage.mss')):
                state = os.path.join(out, 'stage.mss')
            print(f'game over: continuing world {world} from {state}', flush=True)
        elif not os.path.exists(state):
            frame0, state = 0, start
    merge_cdls(out)


if __name__ == '__main__':
    main()
