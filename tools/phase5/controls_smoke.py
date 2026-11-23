#!/usr/bin/env python3
"""Phase 5: play the controls from attract into a race and check what the game decoded.

Runs `dcrun --headless` with a pad script (runtime/pad_script.hpp) that puts in
a coin, presses Start, confirms the menus with the gas (picking MT at car
select with a shift down), then in the race uses every mapped control.
DCUSA_PAD_TRACE=1 prints, whenever the pad changes, the snapshot next to the
game's own decode (PHASE4 "Controls"). The checks:

- coin: the credit count changes (0xFF unset, or n, becomes n + 1)
- start: action 2 is held while Start is down
- gas 0.5 / 1.0 and brake 0.6: the decoded pedal is proportional (0x10000 full)
- steering -1 / +1 / -0.4: full lock reads 0 / 0xFF00, partial lies between
- views 1-4: actions 3-6
- shifting: five ups from first stop at fourth, five downs stop at first
- no fail-closed stop

The script is frame-based. Boot takes about 7,500 presents at 60 Hz and the
menus take fixed times, so the steps land in the right screens. Exit 0 on
success, 1 on a failed check, 77 when the game or cache is missing.

usage: controls_smoke.py --dcrun <exe> --cache <cache> --content <folder> --out <dir>
"""
import argparse
import os
import re
import subprocess
import sys

SCRIPT = """
8000-8003 coin
8200-8205 start
8500-8520 accel=1
8800-8820 accel=1
9030-9040 shiftdown
9100-9120 accel=1
9400-9420 accel=1
9700-9720 accel=1
10000-10020 accel=1
10300-10320 accel=1
11000-11290 accel=1
11300-11360 accel=0.5
11400-11460 brake=0.6
11500-11560 steer=-1
11600-11660 steer=1
11700-11760 steer=-0.4
11800-11810 view1
11860-11870 view2
11920-11930 view3
11980-11990 view4
12040-12050 shiftup
12100-12110 shiftup
12160-12170 shiftup
12220-12230 shiftup
12280-12290 shiftup
12340-12350 shiftdown
12400-12410 shiftdown
12460-12470 shiftdown
12520-12530 shiftdown
12580-12590 shiftdown
"""

LINE = re.compile(r'^pad (\d+): snap btn ([0-9a-f]+) steer ([-+.\d]+) acc ([.\d]+) brk ([.\d]+) \| '
                  r'held ([0-9a-f]+) new ([0-9a-f]+) steer ([0-9a-f]+) acc ([0-9a-f]+) brk ([0-9a-f]+) \| '
                  r'jvs \S+ \S+ \S+ sw \S+ \S+ gear (\d+) credit (\d+)')


def parse(log):
    rows = []
    for l in open(log, errors='replace'):
        m = LINE.match(l)
        if m:
            g = m.groups()
            rows.append({'frame': int(g[0]), 'btn': int(g[1], 16), 'steer_in': float(g[2]), 'acc_in': float(g[3]),
                         'brk_in': float(g[4]), 'held': int(g[5], 16), 'steer': int(g[7], 16),
                         'acc': int(g[8], 16), 'brk': int(g[9], 16), 'gear': int(g[10]), 'credit': int(g[11])})
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dcrun', required=True)
    ap.add_argument('--cache', required=True)
    ap.add_argument('--content', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--seconds', type=int, default=260)
    args = ap.parse_args()
    if not (os.path.exists(args.cache) and os.path.isdir(args.content) and os.path.exists(args.dcrun)):
        print('skip: no prepared cache, game folder or dcrun')
        return 77
    os.makedirs(args.out, exist_ok=True)
    script = os.path.join(args.out, 'pad.txt')
    with open(script, 'w') as f:
        f.write(SCRIPT)
    log = os.path.join(args.out, 'dcrun.log')
    env = dict(os.environ, DCUSA_PAD_SCRIPT=script, DCUSA_PAD_TRACE='1', DCUSA_AUDIO='0')
    with open(log, 'w') as out:
        subprocess.run(['timeout', str(args.seconds), args.dcrun, '--headless', '--shell-setting',
                        'Credit.Freeplay=1', '--content', args.content, args.cache],
                       env=env, stdout=out, stderr=subprocess.STDOUT)
    rows = parse(log)
    failures = []
    text = open(log, errors='replace').read()
    for bad in ('not ported', 'guest fault', 'host fault'):
        if bad in text:
            failures.append('the run stopped: ' + next(l for l in text.splitlines() if bad in l))

    def at(lo, hi, pred=lambda r: True):
        return [r for r in rows if lo <= r['frame'] <= hi and pred(r)]

    def check(ok, what):
        print(('ok    ' if ok else 'FAIL  ') + what)
        if not ok:
            failures.append(what)

    before = at(7900, 7999)
    after = at(8001, 8010)
    check(bool(before and after) and (after[-1]['credit'] == (1 if before[-1]['credit'] == 255 else before[-1]['credit'] + 1)),
          'coin adds a credit')
    check(any(r['held'] & 0x4 for r in at(8201, 8205)), 'start sets action 2')
    half = at(11301, 11360, lambda r: r['acc_in'] == 0.5)
    check(bool(half) and abs(half[-1]['acc'] - 0x8000) <= 0x200, 'gas 0.5 decodes to about 0x8000')
    full = at(11000, 11290, lambda r: r['acc_in'] == 1.0)
    check(bool(full) and full[-1]['acc'] >= 0xFF00, 'gas 1.0 decodes to full')
    brake = at(11401, 11460, lambda r: r['brk_in'] > 0.5)
    check(bool(brake) and abs(brake[-1]['brk'] - int(0.6 * 0x10000)) <= 0x200, 'brake 0.6 decodes to about 0x9999')
    left = at(11501, 11560, lambda r: r['steer_in'] == -1.0)
    right = at(11601, 11660, lambda r: r['steer_in'] == 1.0)
    part = at(11701, 11760, lambda r: abs(r['steer_in'] + 0.4) < 1e-6)
    check(bool(left) and left[-1]['steer'] == 0, 'full left lock reads 0')
    check(bool(right) and right[-1]['steer'] >= 0xFE00, 'full right lock reads 0xFF00')
    check(bool(part) and 0 < part[-1]['steer'] < 0x7F00, 'steering -0.4 lies between left lock and centre')
    for k, (lo, hi) in enumerate(((11801, 11810), (11861, 11870), (11921, 11930), (11981, 11990))):
        check(any(r['held'] & (1 << (3 + k)) for r in at(lo, hi)), 'view %d sets action %d' % (k + 1, 3 + k))
    ups = [at(f + 1, f + 30) for f in (12040, 12100, 12160, 12220, 12280)]
    gears_up = [g[-1]['gear'] if g else None for g in ups]
    downs = [at(f + 1, f + 30) for f in (12340, 12400, 12460, 12520, 12580)]
    gears_down = [g[-1]['gear'] if g else None for g in downs]
    check(gears_up[-1] == 3 and gears_up[-2] == 3, 'shift up stops at fourth (gears after each press: %s)' % gears_up)
    check(gears_down[-1] == 0 and gears_down[-2] == 0, 'shift down stops at first (gears after each press: %s)' % gears_down)
    check(all(b is not None and a is not None and b - a in (0, 1) for a, b in zip(gears_up, gears_up[1:])),
          'each shift up moves at most one gear')
    print('PASS' if not failures else 'FAILED (%d)' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
