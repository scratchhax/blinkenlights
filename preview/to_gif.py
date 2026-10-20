#!/usr/bin/env python3
"""Turn a raw frame dump from `preview --dump` (30 fps, 16x16 RGB) into an animated GIF
that looks like the real panel: round LEDs with a soft glow on black.

    python3 preview/to_gif.py frames.rgb panel.gif
Needs Pillow (`python3 -m pip install pillow`).
"""
import sys

from PIL import Image, ImageDraw, ImageFilter

W = H = 16
PITCH = 14  # pixels per LED
FRAME = W * H * 3


def led_image(raw: bytes) -> Image.Image:
    size = (W * PITCH, H * PITCH)
    dots = Image.new("RGB", size, (0, 0, 0))
    draw = ImageDraw.Draw(dots)
    for y in range(H):
        for x in range(W):
            i = (y * W + x) * 3
            r, g, b = raw[i], raw[i + 1], raw[i + 2]
            # unlit LEDs still show as faint grey dots, like a real panel
            r, g, b = max(r, 14), max(g, 14), max(b, 14)
            cx, cy = x * PITCH + PITCH // 2, y * PITCH + PITCH // 2
            draw.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=(r, g, b))
    glow = dots.filter(ImageFilter.GaussianBlur(3))
    return Image.blend(glow, dots, 0.6).point(lambda v: min(255, int(v * 1.25)))


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    frames = [led_image(data[i:i + FRAME]) for i in range(0, len(data) - FRAME + 1, FRAME)]
    frames = frames[::3]  # 10 fps keeps the file small
    frames[0].save(sys.argv[2], save_all=True, append_images=frames[1:], duration=100, loop=0, optimize=True)
    print(f"{len(frames)} frames -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
