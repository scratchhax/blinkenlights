"""Rough WCAG 2.3.1 / Harding-style flash check on a preview dump (30 fps, 16x16 RGB, perceptual).
Luminance is computed as the panel would show it (firmware gamma 1.6, then sRGB-ish relative luminance).
A flash = a pair of opposing changes in mean window luminance of >= 10 % of max, darker state < 0.8.
Reports the worst flashes-per-second over any 1 s span for every single LED, every 6x6 window
(~25 % of central vision at arm's length) and the whole panel, plus the same for saturated red."""
import sys
GAMMA = 1.6
lut = [max(((i / 255) ** GAMMA), (3 / 255) if i >= 12 else 0) for i in range(256)]
def frames(path):
    d = open(path, 'rb').read()
    return [d[i * 768:(i + 1) * 768] for i in range(len(d) // 768)]
def lum(px, red_only=False):
    r, g, b = lut[px[0]], lut[px[1]], lut[px[2]]
    if red_only:
        s = px[0] + px[1] + px[2]
        if s == 0 or px[0] / s < 0.8:
            return 0.0
    return 0.2126 * r + 0.7152 * g + 0.0722 * b
def series(fr, x0, y0, w, red_only):
    out = []
    for f in fr:
        t = 0.0
        for y in range(y0, y0 + w):
            for x in range(x0, x0 + w):
                i = (y * 16 + x) * 3
                t += lum(f[i:i + 3], red_only)
        out.append(t / (w * w))
    return out
def flashes(sr, fps=30):
    # extrema-based transition counting with 10 % threshold
    ev = []  # times of qualifying transitions
    lo = hi = sr[0]; lo_t = hi_t = 0; direction = 0
    for t, v in enumerate(sr):
        if direction >= 0:
            if v > hi: hi, hi_t = v, t
            if hi - v >= 0.10 and min(hi, v) < 0.8:
                ev.append(t); direction = -1; lo, lo_t = v, t
        if direction <= 0:
            if v < lo: lo, lo_t = v, t
            if v - lo >= 0.10 and min(lo, v) < 0.8:
                ev.append(t); direction = 1; hi, hi_t = v, t
    worst = 0
    for i in range(len(ev)):
        n = sum(1 for e in ev if ev[i] <= e < ev[i] + fps)
        worst = max(worst, n // 2)
    return worst
fr = frames(sys.argv[1])
for red in (False, True):
    led = max(flashes(series(fr, x, y, 1, red)) for y in range(16) for x in range(16))
    wins = max(flashes(series(fr, x, y, 6, red)) for y in range(0, 11, 2) for x in range(0, 11, 2))
    whole = flashes(series(fr, 0, 0, 16, red))
    print(f"{'red   ' if red else 'general'} flashes/s worst: single LED {led}, 6x6 window {wins}, whole panel {whole}")
