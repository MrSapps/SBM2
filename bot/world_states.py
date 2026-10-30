"""Build savestates at the start of each world by entering its password.

The password table (11 x 4 digits at CC:0000, checked by CC:1926; entry n
starts the stage given by the word table at CC:19BE) is read from the ROM,
so nothing ROM-derived is stored in the repo.

Usage: world_states.py [outdir]   (default build/states)
Writes title_menu.mss, password.mss and world<N>.mss (N = 1..5).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, 'bot', 'run.sh')


def passwords():
    rom = open(os.path.join(ROOT, 'baserom.sfc'), 'rb').read()
    table = rom[0xC0000:0xC0000 + 11 * 4]
    stages = rom[0xC19BE:0xC19BE + 11 * 2:2]
    return [(list(table[i * 4:i * 4 + 4]), stages[i]) for i in range(11)]


def run(out, script, load=None, save=None):
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, 'input.txt')
    open(path, 'w').write('\n'.join(script) + '\n')
    env = dict(os.environ, SBM2_LOAD=load or '', SBM2_SAVE=save or '')
    subprocess.run([RUN, out, path, ''], env=env, check=True,
                   stdout=subprocess.DEVNULL)


def presses(buttons, start=10, gap=12):
    lines, f = ['0 -'], start
    for b in buttons:
        lines += [f'{f} {b}', f'{f + 4} -']
        f += gap
    return lines, f


def main():
    states = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build/states')
    work = os.path.join(ROOT, 'build', 'world_states')
    os.makedirs(states, exist_ok=True)
    title = os.path.join(states, 'title_menu.mss')
    pw = os.path.join(states, 'password.mss')
    if not os.path.exists(title):
        run(os.path.join(work, 'title'), ['0 -', '950 start', '954 -', '1300 end'],
            save=f'1290:{title}')
    if not os.path.exists(pw):
        lines, f = presses(['down', 'down', 'start'], gap=30)
        run(os.path.join(work, 'pw'), lines + [f'{f + 330} end'], load=title,
            save=f'{f + 320}:{pw}')
    for n, (digits, stage) in enumerate(passwords()[:5]):
        buttons = []
        for i, d in enumerate(digits):
            buttons += ['up'] * d + (['right'] if i < 3 else [])
        lines, f = presses(buttons + ['start'])
        # Saved on the "N-1 START!" card; the bot waits for the stage itself.
        out = os.path.join(states, f'world{stage >> 4}.mss')
        run(os.path.join(work, f'w{n}'), lines + [f'{f + 210} end'], load=pw,
            save=f'{f + 200}:{out}')
        print(f'world {stage >> 4}: {out}', flush=True)


if __name__ == '__main__':
    main()
