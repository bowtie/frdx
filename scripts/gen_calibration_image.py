#!/usr/bin/env python3
"""Generate calibration images for detecting texture width-stretch.

Produces two PNGs:
  calibration_196.png  — 196x196, fills the full square target texture.
                         Inject in Raw mode to see the game's full UV mapping.
  calibration_104.png  — 104x104, matches the assumed artwork square.
                         Inject in Album-art mode to see if the game
                         samples wider/narrower than 104px.

Both images have:
  - 14px-wide vertical color columns, each a distinct bright color
  - Column index (0-13) drawn large in the center
  - X-pixel labels at the top edge (0, 14, 28, ... or 0, 8, 16, ...)
  - A fine 7px grid in light gray
  - A center crosshair
  - Alternating dark/light rows so vertical stretching is also visible
"""
from PIL import Image, ImageDraw, ImageFont
import colorsys
import os

def make_colors(n):
    """Generate n visually distinct bright colors."""
    colors = []
    for i in range(n):
        h = (i / n) % 1.0
        r, g, b = colorsys.hsv_to_rgb(h, 0.85, 0.95)
        colors.append((int(r*255), int(g*255), int(b*255)))
    return colors

def try_font(size):
    for path in [
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
    ]:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except Exception:
                pass
    return ImageFont.load_default()

def draw_calibration(width, height, out_path):
    img = Image.new("RGBA", (width, height), (20, 20, 20, 255))
    draw = ImageDraw.Draw(img)

    col_w = 14 if width >= 196 else 8
    n_cols = width // col_w
    colors = make_colors(n_cols)

    # Fill vertical color columns
    for c in range(n_cols):
        x0 = c * col_w
        x1 = min(x0 + col_w, width)
        draw.rectangle([x0, 0, x1 - 1, height - 1], fill=colors[c] + (255,))

    # Alternating dark/light horizontal bands (every 14px) to detect vertical stretch
    for y in range(0, height, 14):
        if (y // 14) % 2 == 0:
            # Darken this band slightly
            for c in range(n_cols):
                x0 = c * col_w
                x1 = min(x0 + col_w, width)
                r, g, b = colors[c]
                draw.rectangle([x0, y, x1 - 1, min(y + 14, height) - 1],
                               fill=(r // 2, g // 2, b // 2, 255))

    # Fine grid every 7px (light gray)
    grid_color = (255, 255, 255, 60)
    for x in range(0, width, 7):
        draw.line([(x, 0), (x, height - 1)], fill=grid_color, width=1)
    for y in range(0, height, 7):
        draw.line([(0, y), (width - 1, y)], fill=grid_color, width=1)

    # Bold grid every 14px (white)
    bold_color = (255, 255, 255, 120)
    for x in range(0, width, col_w):
        draw.line([(x, 0), (x, height - 1)], fill=bold_color, width=1)
    for y in range(0, height, 14):
        draw.line([(0, y), (width - 1, y)], fill=bold_color, width=1)

    # X-pixel labels at the top
    font_small = try_font(8 if width <= 104 else 9)
    for c in range(n_cols + 1):
        x = c * col_w
        if x >= width:
            break
        label = str(x)
        draw.text((x + 1, 1), label, fill=(255, 255, 255, 255), font=font_small)

    # Column index numbers, large, centered in each column
    font_big = try_font(10 if width <= 104 else 12)
    for c in range(n_cols):
        cx = c * col_w + col_w // 2
        cy = height // 2
        label = str(c)
        bbox = draw.textbbox((0, 0), label, font=font_big)
        tw = bbox[2] - bbox[0]
        th = bbox[3] - bbox[1]
        # Draw with a dark background for readability
        draw.rectangle([cx - tw // 2 - 1, cy - th // 2 - 1,
                        cx + tw // 2 + 1, cy + th // 2 + 1],
                       fill=(0, 0, 0, 200))
        draw.text((cx - tw // 2, cy - th // 2), label,
                  fill=(255, 255, 255, 255), font=font_big)

    # Center crosshair
    cx, cy = width // 2, height // 2
    draw.line([(cx - 5, cy), (cx + 5, cy)], fill=(255, 255, 0, 255), width=1)
    draw.line([(cx, cy - 5), (cx, cy + 5)], fill=(255, 255, 0, 255), width=1)

    # Border
    draw.rectangle([0, 0, width - 1, height - 1], outline=(255, 255, 255, 255))

    img.save(out_path)
    print(f"Wrote {out_path}  ({width}x{height}, {n_cols} columns of {col_w}px)")

if __name__ == "__main__":
    out_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(os.path.dirname(out_dir), "assets")
    os.makedirs(out_dir, exist_ok=True)

    draw_calibration(196, 196, os.path.join(out_dir, "calibration_196x196.png"))
    draw_calibration(196, 104, os.path.join(out_dir, "calibration_196x104.png"))
    draw_calibration(104, 104, os.path.join(out_dir, "calibration_104x104.png"))
