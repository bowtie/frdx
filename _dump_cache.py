#!/usr/bin/env python3
"""Convert BC7 cache files to PNG images for inspection.

Reads each .bc7 file from the artwork_cache directory, wraps the BC7
payload in a DDS header, and uses texconv to convert it to PNG.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

# DDS header constants
DDS_MAGIC = b"DDS "
DDS_HEADER_SIZE = 124
DDS_PF_SIZE = 32
DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PIXELFORMAT = 0x1000
DDSD_LINEARSIZE = 0x80000
DDPF_FOURCC = 0x4
BC7_FOURCC = b"DX10"

# DX10 header extension for BC7
DX10_HEADER_SIZE = 20
DXGI_FORMAT_BC7_UNORM = 98
D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3
D3D10_RESOURCE_MISC_FLAG_NONE = 0
DDS_RESOURCE_MISC_TEXTURECUBE = 0x4
DDSCAPS_TEXTURE = 0x1000


def make_dds(w: int, h: int, bc7_payload: bytes) -> bytes:
    """Wrap BC7 payload in a DDS file."""
    # DDS pixel format (32 bytes)
    pf = struct.pack(
        "<II4sIIIII",
        DDS_PF_SIZE,       # dwSize
        DDPF_FOURCC,       # dwFlags
        BC7_FOURCC,        # dwFourCC = "DX10"
        0,                 # dwRGBBitCount
        0, 0, 0, 0,        # RGBA masks
    )

    # DDS header (124 bytes)
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE
    pitch = max(1, ((w + 3) // 4)) * 16  # BC7: 16 bytes per 4x4 block

    # Build header as explicit bytes to avoid format string miscounts
    header = DDS_MAGIC
    header += struct.pack("<I", DDS_HEADER_SIZE)
    header += struct.pack("<I", flags)
    header += struct.pack("<I", h)
    header += struct.pack("<I", w)
    header += struct.pack("<I", pitch)
    header += struct.pack("<I", 0)   # dwDepth
    header += struct.pack("<I", 1)   # dwMipMapCount
    header += b"\x00" * 44           # dwReserved1[11] (11 * 4 bytes)
    header += pf                      # DDS_PIXELFORMAT (32 bytes)
    header += struct.pack("<I", DDSCAPS_TEXTURE)  # dwCaps
    header += struct.pack("<I", 0)   # dwCaps2
    header += struct.pack("<I", 0)   # dwCaps3
    header += struct.pack("<I", 0)   # dwCaps4
    header += struct.pack("<I", 0)   # dwReserved2

    # DX10 extension (20 bytes — all DWORDs)
    dx10 = struct.pack(
        "<IIIII",
        DXGI_FORMAT_BC7_UNORM,
        D3D10_RESOURCE_DIMENSION_TEXTURE2D,
        D3D10_RESOURCE_MISC_FLAG_NONE,
        1,   # arraySize
        0,   # miscFlags2
    )

    return header + dx10 + bc7_payload


def convert_bc7_to_png(bc7_path: Path, texconv: Path, out_dir: Path) -> bool:
    """Convert a single .bc7 cache file to PNG."""
    with open(bc7_path, "rb") as f:
        data = f.read()

    if len(data) < 12:
        print(f"  SKIP (too small): {bc7_path.name}")
        return False

    w, h, payload_size = struct.unpack_from("<III", data, 0)
    if w == 0 or h == 0 or payload_size == 0:
        print(f"  SKIP (bad header): {bc7_path.name}")
        return False

    if len(data) < 12 + payload_size:
        print(f"  SKIP (truncated): {bc7_path.name}")
        return False

    payload = data[12 : 12 + payload_size]

    # Write DDS
    dds_path = out_dir / (bc7_path.stem + ".dds")
    with open(dds_path, "wb") as f:
        f.write(make_dds(w, h, payload))

    # Convert DDS -> PNG with texconv
    # -pmalpha: un-premultiply alpha (compression used -pmalpha, so
    # decompression must also use it to restore correct RGB values)
    png_path = out_dir / (bc7_path.stem + ".png")
    cmd = [
        str(texconv),
        "-ft", "png",
        "-pmalpha",
        "-o", str(out_dir),
        "-y",
        str(dds_path),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)

    # Clean up DDS
    dds_path.unlink(missing_ok=True)

    if png_path.exists():
        print(f"  OK: {bc7_path.name} -> {png_path.name} ({w}x{h})")
        return True
    else:
        print(f"  FAIL: {bc7_path.name} ({w}x{h})")
        if result.stderr:
            print(f"    stderr: {result.stderr.strip()}")
        return False


def main():
    parser = argparse.ArgumentParser(description="Convert BC7 cache files to PNG")
    parser.add_argument(
        "--cache-dir",
        default=r"S:\SteamLibrary\steamapps\common\ForzaHorizon6\mods\frdx\artwork_cache",
        help="Path to artwork_cache directory",
    )
    parser.add_argument(
        "--texconv",
        default=r"S:\SteamLibrary\steamapps\common\ForzaHorizon6\mods\frdx\bin\texconv.exe",
        help="Path to texconv.exe",
    )
    parser.add_argument(
        "--out-dir",
        default=None,
        help="Output directory for PNGs (default: <cache-dir>/_preview)",
    )
    args = parser.parse_args()

    cache_dir = Path(args.cache_dir)
    if not cache_dir.exists():
        print(f"Cache directory not found: {cache_dir}")
        sys.exit(1)

    texconv = Path(args.texconv)
    if not texconv.exists():
        print(f"texconv not found: {texconv}")
        sys.exit(1)

    out_dir = Path(args.out_dir) if args.out_dir else cache_dir / "_preview"
    out_dir.mkdir(parents=True, exist_ok=True)

    bc7_files = sorted(cache_dir.glob("*.bc7"))
    if not bc7_files:
        print(f"No .bc7 files in {cache_dir}")
        sys.exit(0)

    print(f"Converting {len(bc7_files)} cache file(s) to PNG...")
    ok = 0
    for bc7 in bc7_files:
        if convert_bc7_to_png(bc7, texconv, out_dir):
            ok += 1

    print(f"\nDone: {ok}/{len(bc7_files)} converted -> {out_dir}")


if __name__ == "__main__":
    main()
