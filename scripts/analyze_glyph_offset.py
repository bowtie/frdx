#!/usr/bin/env python3
"""
Analyze player bar screenshots to measure glyph vertical alignment.

Detects the player bar region, finds transport buttons, and measures
how glyphs (icons/text) are positioned relative to button centers.
"""

import sys
import os
import numpy as np
from PIL import Image


def load_rgb(path):
    """Load image as RGB numpy array."""
    img = Image.open(path).convert("RGB")
    return np.array(img), img.size


def find_player_bar(rgb):
    """
    Find the player bar region at the top of the screen.
    The player bar has a distinct background color (dark panel).
    Returns (y_top, y_bottom) of the entire player bar.
    """
    h, w, _ = rgb.shape
    gray = rgb.mean(axis=2)
    row_mean = gray.mean(axis=1)

    # The player bar is a dark band near the top.
    # Find the first transition from light (scene/titlebar) to dark (bar).
    # Then find where it transitions back to lighter (scene below).
    median = np.median(row_mean[:200])

    # Find dark band starting from top
    dark_threshold = median - 30
    in_bar = False
    bar_start = 0
    bar_end = 0

    for y in range(min(400, h)):
        if row_mean[y] < dark_threshold:
            if not in_bar:
                bar_start = y
                in_bar = True
        else:
            if in_bar:
                # Check if this is a real exit (sustained lighter rows)
                if y + 5 < h and all(row_mean[y + k] > dark_threshold + 10 for k in range(5)):
                    bar_end = y
                    break

    if not in_bar:
        return 0, 0
    if bar_end == 0:
        bar_end = min(bar_start + 200, h)

    return bar_start, bar_end


def find_button_subregion(rgb, bar_top, bar_bottom):
    """
    Within the player bar, find the sub-region containing transport buttons.
    Buttons have a slightly different shade from the bar background.
    Returns (btn_y_top, btn_y_bottom) relative to bar_top.
    """
    bar = rgb[bar_top:bar_bottom, :, :]
    gray = bar.mean(axis=2)
    h = gray.shape[0]

    # Row variance — button rows have higher variance (icon + background)
    row_var = gray.var(axis=1)
    # Find the peak variance region (that's where the buttons are)
    # Smooth the variance
    if h > 10:
        kernel = np.ones(5) / 5
        row_var_smooth = np.convolve(row_var, kernel, mode='same')
    else:
        row_var_smooth = row_var

    # Find the region with highest variance
    threshold = np.max(row_var_smooth) * 0.3
    high_var = row_var_smooth > threshold
    rows = np.where(high_var)[0]
    if len(rows) == 0:
        return 0, h

    return rows[0], rows[-1] + 1


def find_buttons(rgb, bar_top, btn_y_top, btn_y_bottom, bar_w_start=0):
    """
    Find individual buttons by looking for gaps in the button row.
    Returns list of (x_left, x_right) for each button.
    """
    abs_top = bar_top + btn_y_top
    abs_bot = bar_top + btn_y_bottom
    band = rgb[abs_top:abs_bot, :, :].mean(axis=2)

    # Column profile
    col_mean = band.mean(axis=0)
    w = len(col_mean)

    # Find the button area — skip empty margins
    # Buttons are in the center-left area
    median = np.median(col_mean)

    # Find gaps (columns that match the bar background, not button bg)
    # Buttons have a slightly different shade
    col_var = band.var(axis=0)

    # Find segments with content (higher variance = glyphs)
    # vs empty padding (low variance)
    var_threshold = np.max(col_var) * 0.05
    has_content = col_var > var_threshold

    # Find button groups — clusters of content columns separated by gaps
    buttons = []
    in_btn = False
    btn_start = 0
    min_gap = 3

    for x in range(w):
        if has_content[x]:
            if not in_btn:
                btn_start = x
                in_btn = True
        else:
            if in_btn:
                # Check if this gap is sustained
                gap_len = 0
                for k in range(x, min(x + min_gap + 1, w)):
                    if not has_content[k]:
                        gap_len += 1
                    else:
                        break
                if gap_len >= min_gap:
                    btn_w = x - btn_start
                    if btn_w > 10:
                        buttons.append((btn_start, x))
                    in_btn = False

    if in_btn:
        btn_w = w - btn_start
        if btn_w > 10:
            buttons.append((btn_start, w))

    return buttons


def find_glyph_vertical_bounds(rgb, x_left, x_right, y_top, y_bottom):
    """
    Find vertical bounds of glyph pixels within a button region.
    Glyphs differ from the button background.
    Returns (glyph_top, glyph_bottom) relative to y_top, or None.
    """
    region = rgb[y_top:y_bottom, x_left:x_right, :].mean(axis=2)
    if region.size == 0:
        return None

    h, w = region.shape
    # Background is the most common color (median)
    bg = np.median(region)

    # Glyphs are either darker or lighter than background
    # Check center pixels to determine direction
    center = region[h//4:3*h//4, w//4:3*w//4]
    center_mean = center.mean()

    if center_mean < bg:
        glyph_mask = region < (bg - 15)
    else:
        glyph_mask = region > (bg + 15)

    if not glyph_mask.any():
        return None

    # Find rows with glyph pixels, but ignore edge noise (1-2 pixels)
    row_counts = glyph_mask.sum(axis=1)
    # Only count rows with at least 2 glyph pixels (filters noise)
    row_has_glyph = row_counts >= 2
    glyph_rows = np.where(row_has_glyph)[0]

    if len(glyph_rows) == 0:
        return None

    return glyph_rows[0], glyph_rows[-1]


def analyze_screenshot(path):
    """Analyze a single screenshot."""
    rgb, (w, h) = load_rgb(path)
    bar_top, bar_bottom = find_player_bar(rgb)

    if bar_top == bar_bottom:
        print(f"\n{'='*60}")
        print(f"Screenshot: {os.path.basename(path)}")
        print(f"  Could not detect player bar!")
        return None

    btn_y_top, btn_y_bottom = find_button_subregion(rgb, bar_top, bar_bottom)
    buttons = find_buttons(rgb, bar_top, btn_y_top, btn_y_bottom)

    btn_height = btn_y_bottom - btn_y_top
    btn_center = btn_height / 2.0

    print(f"\n{'='*60}")
    print(f"Screenshot: {os.path.basename(path)}")
    print(f"Image: {w}x{h}")
    print(f"Player bar: y={bar_top}..{bar_bottom}")
    print(f"Button row: y={bar_top+btn_y_top}..{bar_top+btn_y_bottom} (height={btn_height}px)")
    print(f"Buttons found: {len(buttons)}")
    print(f"{'='*60}")

    offsets = []
    for i, (x_left, x_right) in enumerate(buttons):
        btn_w = x_right - x_left
        abs_top = bar_top + btn_y_top
        abs_bot = bar_top + btn_y_bottom
        result = find_glyph_vertical_bounds(rgb, x_left, x_right, abs_top, abs_bot)

        if result is None:
            print(f"  Button {i}: x={x_left}..{x_right} ({btn_w}px) -- no glyph")
            continue

        glyph_top, glyph_bottom = result
        glyph_h = glyph_bottom - glyph_top + 1
        glyph_center = (glyph_top + glyph_bottom) / 2.0
        offset = glyph_center - btn_center

        direction = 'below' if offset > 0 else 'above'
        print(f"  Button {i}: x={x_left}..{x_right} ({btn_w}px)")
        print(f"    Glyph: y={glyph_top}..{glyph_bottom} (h={glyph_h}px)")
        print(f"    Btn center={btn_center:.1f}, Glyph center={glyph_center:.1f}")
        print(f"    Offset: {offset:+.1f}px ({direction} by {abs(offset):.1f}px)")

        offsets.append(offset)

    if offsets:
        avg = np.mean(offsets)
        print(f"\n  Avg offset: {avg:+.1f}px")
        print(f"  Recommended GlyphOffset.y: {-avg:.1f}")

    return offsets


def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_glyph_offset.py <screenshot.png> [...]")
        sys.exit(1)

    all_results = {}
    for path in sys.argv[1:]:
        if not os.path.exists(path):
            print(f"Error: {path} not found")
            continue
        offsets = analyze_screenshot(path)
        if offsets is not None:
            all_results[os.path.basename(path)] = offsets

    if len(all_results) > 1:
        print(f"\n\n{'='*60}")
        print("SUMMARY")
        print(f"{'='*60}")
        for name, offs in all_results.items():
            if offs:
                avg = np.mean(offs)
                print(f"  {name:30s}  avg={avg:+.1f}px  ->  GlyphOffset.y={-avg:.1f}")


if __name__ == "__main__":
    main()
