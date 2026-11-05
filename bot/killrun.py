"""Stop the bot processes of one run or bench: every drive.py / botrun /
bench.py whose SBM2_OUT (or command line, for bench.py) is under OUT.
Matches by /proc/<pid>/environ, so the calling shell (whose command line
names OUT too) is never hit.

Usage: killrun.py OUT_DIR
"""
import os
import signal
import sys


def main():
    out = os.path.abspath(sys.argv[1])
    me = {os.getpid(), os.getppid()}
    hits = []
    for pid in os.listdir('/proc'):
        if not pid.isdigit() or int(pid) in me:
            continue
        try:
            cmd = open(f'/proc/{pid}/cmdline', 'rb').read().split(b'\0')
            env = open(f'/proc/{pid}/environ', 'rb').read().split(b'\0')
        except OSError:
            continue
        exe = os.path.basename(cmd[0].decode(errors='replace')) if cmd and cmd[0] else ''
        args = [c.decode(errors='replace') for c in cmd]
        is_bot = exe == 'botrun' or any(a.endswith('drive.py') or a.endswith('bench.py') for a in args)
        if not is_bot:
            continue
        outs = [e.decode(errors='replace')[9:] for e in env if e.startswith(b'SBM2_OUT=')]
        if any(o.startswith(out) for o in outs) or any(os.path.abspath(a).startswith(out) for a in args[1:] if '/' in a):
            hits.append(int(pid))
    # drivers first, so they don't restart the botruns
    hits.sort(key=lambda p: 0 if b'botrun' not in open(f'/proc/{p}/cmdline', 'rb').read() else 1)
    for p in hits:
        try:
            os.kill(p, signal.SIGTERM)
        except OSError:
            pass
    print(f'stopped {len(hits)} processes under {out}')


if __name__ == '__main__':
    main()
