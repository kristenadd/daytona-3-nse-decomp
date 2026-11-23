#!/usr/bin/env python3
"""Phase 5: run the attract loop and compare it with a Windows capture.

The two loops are not frame-synchronous (camera timing differs run to run), so
every frame of ours is matched to the most similar reference frame and judged
against that one:

- colour: a 48-bin histogram (16 per channel), L1 distance between normalised
  histograms (0 = same colours, 2 = disjoint). It finds the candidates and is
  the measure for 3D scenes, whose camera differs.
- picture: mean absolute difference of 32x18 thumbnails (0..255), against the
  five closest candidates by colour. 2D screens (logos, title, cards) should
  match closely.
- sanity: no frame after attract starts is black or has a magenta region (the
  NaN bloom of PHASE3).
- overall: median luma and saturation of all attract frames, ours against the
  capture's.

Frames come from `dcrun --headless` (DCUSA_FRAME every 120 presents, 2 s at
60 Hz) unless --frames gives a folder of o.<present>.ppm / .png from an
earlier run. The capture is sampled every 2 s. Boot frames (black, text on
black) are left out on both sides.

Exit 0 when every check passes, 1 when one fails, 77 when an input is
missing (CTest's skip). Writes <out>/report.json and <out>/worst.png (the
worst-matching frames beside their reference).

usage: attract_compare.py --reference <video> [--dcrun <exe> --cache <cache>
       --content <folder> | --frames <dir>] --out <dir> [--seconds 690]
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

THUMB_W, THUMB_H = 32, 18

# Tolerances, from the build that matched the capture (PHASE3 colour check),
# with headroom: the capture is lossy H.264 and the loops differ in timing.
# Colour distance to the best match. A few camera shots (a wheel filling the
# frame) have no counterpart in a capture sampled every 2 s, so a single worst
# frame is not a measure of colour; the 95th percentile is. Measured: the
# matching build 0.45, the magenta-bloom build 0.81.
P95_COLOUR = 0.60
MAX_COLOUR = 0.80          # any single frame, a loose backstop
MEDIAN_COLOUR = 0.30       # median over frames
MAX_LUMA_DIFF = 12.0       # median luma, ours vs capture (0..255)
MAX_SAT_DIFF = 0.06        # median saturation, ours vs capture (0..1)
MIN_ATTRACT_FRAMES = 150   # a full loop at 2 s per frame is about 250


def ffmpeg_raw(args, w, h):
    """Run ffmpeg with `args` (inputs and filters) writing rgb24 frames of w x h; yields bytes per frame."""
    cmd = ['ffmpeg', '-loglevel', 'error'] + args + ['-vf', 'scale=%d:%d:flags=area' % (w, h), '-f', 'rawvideo',
                                                    '-pix_fmt', 'rgb24', '-']
    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    n = w * h * 3
    return [out[i:i + n] for i in range(0, len(out) - n + 1, n)]


def reference_thumbs(video):
    return ffmpeg_raw(['-i', video, '-r', '0.5'], THUMB_W, THUMB_H)


def frame_files(folder):
    found = []
    for name in os.listdir(folder):
        m = re.match(r'(?:o\.|f_|s_)(\d+)\.(ppm|png)$', name)
        if m:
            found.append((int(m.group(1)), os.path.join(folder, name)))
    best = {}
    for n, path in found:  # prefer full-size files when both exist
        if n not in best or path.endswith('.ppm') or os.path.basename(path).startswith('f_'):
            best[n] = path
    return sorted(best.items())


def thumb_of(path):
    return ffmpeg_raw(['-i', path], THUMB_W, THUMB_H)[0]


def stats(px):
    hist = [0] * 48
    luma = sat = 0.0
    n = len(px) // 3
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        hist[r >> 4] += 1
        hist[16 + (g >> 4)] += 1
        hist[32 + (b >> 4)] += 1
        luma += 0.2126 * r + 0.7152 * g + 0.0722 * b
        mx = max(r, g, b)
        sat += (mx - min(r, g, b)) / mx if mx else 0.0
    return [h / n for h in hist], luma / n, sat / n


def is_boot(px, luma):
    """Black boot screens (status text on black) and empty frames."""
    return luma < 12.0


def magenta_fraction(px):
    n = len(px) // 3
    m = sum(1 for i in range(0, len(px), 3) if px[i] > 200 and px[i + 2] > 200 and px[i + 1] < 90)
    return m / n


def hist_dist(a, b):
    """L1 distance of the three normalised channel histograms, averaged: 0..2."""
    return sum(abs(x - y) for x, y in zip(a, b)) / 3.0


def pixel_dist(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def median(v):
    v = sorted(v)
    return v[len(v) // 2] if v else 0.0


def run_game(args, frames_dir):
    os.makedirs(frames_dir, exist_ok=True)
    env = dict(os.environ, DCUSA_PAD='0', DCUSA_AUDIO='0', DCUSA_EVERY='120',
               DCUSA_FRAME=os.path.join(frames_dir, 'o'))
    cmd = ['timeout', str(args.seconds), args.dcrun, '--headless', '--content', args.content, args.cache]
    with open(os.path.join(args.out, 'dcrun.log'), 'w') as log:
        subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    return frames_dir


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--reference', required=True)
    ap.add_argument('--dcrun')
    ap.add_argument('--cache')
    ap.add_argument('--content')
    ap.add_argument('--frames', help='use frames from an earlier run instead of running the game')
    ap.add_argument('--out', required=True)
    ap.add_argument('--seconds', type=int, default=690)
    args = ap.parse_args()
    if not os.path.exists(args.reference) or shutil.which('ffmpeg') is None:
        print('skip: no reference capture or no ffmpeg')
        return 77
    os.makedirs(args.out, exist_ok=True)
    if args.frames:
        frames_dir = args.frames
    else:
        if not (args.dcrun and args.cache and args.content and os.path.exists(args.cache)
                and os.path.isdir(args.content)):
            print('skip: no prepared cache or game folder')
            return 77
        frames_dir = run_game(args, os.path.join(args.out, 'frames'))

    ref = []
    for px in reference_thumbs(args.reference):
        hist, luma, sat = stats(px)
        if not is_boot(px, luma):
            ref.append((px, hist, luma, sat))
    ours = []
    for n, path in frame_files(frames_dir):
        px = thumb_of(path)
        hist, luma, sat = stats(px)
        ours.append((n, path, px, hist, luma, sat))
    # Attract starts at the first frame that is not boot; everything after counts.
    first = next((i for i, o in enumerate(ours) if not is_boot(o[2], o[4])), len(ours))
    attract = ours[first:]

    failures = []
    rows = []
    for n, path, px, hist, luma, sat in attract:
        ranked = sorted(range(len(ref)), key=lambda k: hist_dist(hist, ref[k][1]))[:5]
        best = min(ranked, key=lambda k: pixel_dist(px, ref[k][0]))
        rows.append({'present': n, 'file': path, 'colour': round(hist_dist(hist, ref[ranked[0]][1]), 4),
                     'picture': round(pixel_dist(px, ref[best][0]), 2), 'ref_index': best,
                     'luma': round(luma, 1), 'magenta': round(magenta_fraction(px), 4)})
        if is_boot(px, luma):
            failures.append('present %d is black after attract started' % n)
        if rows[-1]['magenta'] > 0.03:
            failures.append('present %d has a magenta region (%.1f%%)' % (n, 100 * rows[-1]['magenta']))

    colours = [r['colour'] for r in rows]
    ranked_colours = sorted(colours)
    p95 = ranked_colours[int(0.95 * (len(ranked_colours) - 1))] if ranked_colours else 0.0
    summary = {
        'attract_frames': len(rows),
        'colour_p95': p95,
        'reference_frames': len(ref),
        'colour_worst': max(colours) if colours else None,
        'colour_median': median(colours),
        'picture_median': median([r['picture'] for r in rows]),
        'luma_median': {'ours': round(median([o[4] for o in attract]), 1), 'reference': round(median([r[2] for r in ref]), 1)},
        'saturation_median': {'ours': round(median([o[5] for o in attract]), 3),
                              'reference': round(median([r[3] for r in ref]), 3)},
    }
    if len(rows) < MIN_ATTRACT_FRAMES:
        failures.append('only %d attract frames (want %d): the run did not reach or stay in attract' % (
            len(rows), MIN_ATTRACT_FRAMES))
    if colours and p95 > P95_COLOUR:
        failures.append('95th percentile colour distance %.3f > %.2f' % (p95, P95_COLOUR))
    if colours and max(colours) > MAX_COLOUR:
        failures.append('worst colour distance %.3f > %.2f' % (max(colours), MAX_COLOUR))
    if colours and median(colours) > MEDIAN_COLOUR:
        failures.append('median colour distance %.3f > %.2f' % (median(colours), MEDIAN_COLOUR))
    dl = abs(summary['luma_median']['ours'] - summary['luma_median']['reference'])
    if dl > MAX_LUMA_DIFF:
        failures.append('median luma differs by %.1f > %.1f' % (dl, MAX_LUMA_DIFF))
    ds = abs(summary['saturation_median']['ours'] - summary['saturation_median']['reference'])
    if ds > MAX_SAT_DIFF:
        failures.append('median saturation differs by %.3f > %.2f' % (ds, MAX_SAT_DIFF))
    summary['failures'] = failures
    summary['frames'] = rows

    # The six worst frames beside their reference, for a look.
    worst = sorted(rows, key=lambda r: -r['colour'])[:6]
    if worst:
        inputs = []
        for k, r in enumerate(worst):
            inputs += ['-i', r['file']]
        # reference frames are re-extracted at full size by index (2 s apart, boot skipped)
        ref_full = os.path.join(args.out, 'ref_full')
        os.makedirs(ref_full, exist_ok=True)
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', args.reference, '-r', '0.5', '-vf', 'scale=480:270',
                        os.path.join(ref_full, 'r_%04d.png')], check=True)
        all_ref = sorted(os.listdir(ref_full))
        nonboot = []
        for name in all_ref:
            px = thumb_of(os.path.join(ref_full, name))
            if not is_boot(px, stats(px)[1]):
                nonboot.append(os.path.join(ref_full, name))
        for k, r in enumerate(worst):
            inputs += ['-i', nonboot[r['ref_index']]]
        m = len(worst)
        parts = []
        for k in range(m):
            parts.append('[%d:v]scale=480:270[o%d];[%d:v]scale=480:270[r%d];[o%d][r%d]hstack[h%d]' % (k, k, m + k, k, k, k, k))
        graph = ';'.join(parts) + ';' + ''.join('[h%d]' % k for k in range(m)) + 'vstack=inputs=%d[out]' % m
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + inputs + ['-filter_complex', graph, '-map', '[out]',
                                                                          os.path.join(args.out, 'worst.png')])
    with open(os.path.join(args.out, 'report.json'), 'w') as f:
        json.dump(summary, f, indent=1)
    print('attract frames %d (reference %d)' % (summary['attract_frames'], summary['reference_frames']))
    print('colour distance median %.3f p95 %.3f worst %.3f; picture median %.1f' % (
        summary['colour_median'], p95, summary['colour_worst'] or 0, summary['picture_median']))
    print('median luma ours %.1f capture %.1f; saturation ours %.3f capture %.3f' % (
        summary['luma_median']['ours'], summary['luma_median']['reference'], summary['saturation_median']['ours'],
        summary['saturation_median']['reference']))
    for f in failures:
        print('FAIL: ' + f)
    print('PASS' if not failures else 'FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
