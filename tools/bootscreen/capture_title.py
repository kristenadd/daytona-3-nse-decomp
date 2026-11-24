#!/usr/bin/env python3
"""Render the attract title screen without "PRESS START BUTTON" / "FREE PLAY".

The boot screen (DCUSA_BOOT_SCREEN) and the iOS launch image show the title
art the game draws in attract. It comes from the player's own game files, so
it is made here, on the Mac, and never committed:

    tools/bootscreen/capture_title.py --dcrun build/mac-host/dcrun \\
        --cache build/mac-host/prepared/DCUSA2019.cache \\
        --content "../Daytona Championship USA" --out build/mac-host/prepared/boot/title.png

dcrun runs attract headless with the overlay text left out: the two lines are
one 1504x264 image drawn with the sprite shader 9fc2a89a0b7a4258 into the
backbuffer (DCUSA_SKIP_PS_RT, DCUSA_SKIP_SIZES; the copyright, 1924x1928,
stays). A frame every 60 presents is kept; the title is the first frame
that is about 72% white and still the same 60 presents later (the logo
animates in before that).
"""
import argparse, glob, os, re, shutil, subprocess, sys, tempfile

def white(path):
    px = open(path, 'rb').read().split(b'\n', 3)[3]
    s = px[::3 * 97]
    return sum(1 for v in s if v > 235) / len(s)

def same(a, b):
    pa = open(a, 'rb').read().split(b'\n', 3)[3]
    pb = open(b, 'rb').read().split(b'\n', 3)[3]
    if len(pa) != len(pb): return False
    step = 3 * 101
    diff = sum(abs(pa[i] - pb[i]) for i in range(0, len(pa), step))
    return diff / (len(pa) // step) < 2.0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dcrun', required=True)
    ap.add_argument('--cache', required=True)
    ap.add_argument('--content', required=True)
    ap.add_argument('--shaders', help='prepared shader folder (default: beside the cache)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--presents', type=int, default=22000)
    a = ap.parse_args()
    a.dcrun, a.cache, a.content, a.out = (os.path.abspath(p) for p in (a.dcrun, a.cache, a.content, a.out))
    shaders = a.shaders or os.path.join(os.path.dirname(os.path.abspath(a.cache)), 'shaders')
    with tempfile.TemporaryDirectory() as work:
        # A fresh container: the run must not write into the real one.
        os.makedirs(os.path.join(work, 'c'))
        cache = os.path.join(work, 'c', os.path.basename(a.cache))
        shutil.copy(a.cache, cache)
        env = dict(os.environ, DCUSA_SKIP_PS_RT='9fc2a89a0b7a4258@e0000180', DCUSA_SKIP_WIDTH='0',
                   DCUSA_SKIP_SIZES='1504x264', DCUSA_BOOT_PACE='1', DCUSA_SHADERS=shaders, DCUSA_PAD='0',
                   DCUSA_AUDIO='0', DCUSA_FRAME=os.path.join(work, 'f'), DCUSA_EVERY='60',
                   DCUSA_EXIT_AT=str(a.presents))
        subprocess.run([a.dcrun, '--headless', '--content', a.content, cache], env=env, cwd=work,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        frames = sorted(glob.glob(os.path.join(work, 'f.*.ppm')), key=lambda f: int(re.search(r'f\.(\d+)', f).group(1)))
        frames = [f for f in frames if int(re.search(r'f\.(\d+)', f).group(1)) > 8000]
        for f, g in zip(frames, frames[1:]):
            if 0.70 < white(f) < 0.75 and same(f, g):
                os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
                subprocess.run(['sips', '-s', 'format', 'png', f, '--out', a.out], stdout=subprocess.DEVNULL, check=True)
                print('title from', os.path.basename(f), '->', a.out)
                return 0
    print('no settled title frame found', file=sys.stderr)
    return 1

if __name__ == '__main__':
    sys.exit(main())
