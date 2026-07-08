#!/usr/bin/env python3
"""Pre-warm the BC7 disk cache for all station logos.

Reads stations.json, finds all local logo paths, and runs texconv to
produce BC7 cache files matching the format used by the TextureInjector.
"""
import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

# FNV-1a 64-bit hash (matches url_hash_fnv1a in texture_injector.cpp)
def fnv1a_64(s: str) -> int:
    h = 0xcbf29ce484222325
    for c in s.encode('utf-8'):
        h ^= c
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

# FNV-1a 64-bit hash of file contents (matches compute_file_hash)
def fnv1a_64_file(path: Path) -> int:
    h = 0xcbf29ce484222325
    with open(path, 'rb') as f:
        while True:
            buf = f.read(8192)
            if not buf:
                break
            for b in buf:
                h ^= b
                h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def collect_logo_urls(stations_path: Path) -> list[str]:
    """Extract all local logo paths from stations.json."""
    with open(stations_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    urls = []
    if isinstance(data, dict):
        data = [data]

    for station in data:
        for stream in station.get('streams', []):
            url = stream.get('artwork_url', '')
            if url and not url.startswith('http') and '/' not in url or \
               (url and url.startswith('logos/')):
                urls.append(url)
        meta = station.get('metadata', {})
        url = meta.get('static_artwork_url', '')
        if url:
            urls.append(url)

    # Deduplicate
    urls = sorted(set(urls))
    return urls


def process_logo(logo_url: str, data_dir: Path, texconv: Path,
                 cache_dir: Path, target_w: int, target_h: int) -> bool:
    """Process a single logo: load -> resize -> texconv BC7 -> save cache."""
    local_path = data_dir / logo_url
    if not local_path.exists():
        print(f"  SKIP (not found): {logo_url}")
        return False

    # Compute file hash
    file_hash = fnv1a_64_file(local_path)

    # Check if cache already exists with matching hash
    url_h = fnv1a_64(logo_url)
    cache_name = f"{url_h}_{file_hash}_{target_w}x{target_h}.bc7"
    cache_file = cache_dir / cache_name
    if cache_file.exists():
        print(f"  SKIP (cached): {logo_url}")
        return True

    # Use stb_image via a tiny C program? No — use texconv directly.
    # texconv can load PNG/JPG directly and output BC7 DDS.
    # Then we extract the BC7 payload from the DDS.

    # Create temp dir
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        dds_path = tmpdir / "logo.dds"

        # Run texconv: load image, resize to target, BC7 compress
        cmd = [
            str(texconv),
            "-f", "BC7_UNORM",
            "-w", str(target_w),
            "-h", str(target_h),
            "-m", "1",
            "-pmalpha",
            "-gpu", "0",
            "-y",
            "-o", str(tmpdir),
            str(local_path),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if not dds_path.exists():
            # texconv outputs with original name + .dds
            dds_path = tmpdir / (local_path.stem + ".dds")
        if not dds_path.exists():
            print(f"  FAIL (texconv): {logo_url}")
            if result.stderr:
                print(f"    {result.stderr.strip()}")
            return False

        # Read DDS and extract BC7 payload
        dds_data = dds_path.read_bytes()
        if len(dds_data) < 148:
            print(f"  FAIL (DDS too small): {logo_url}")
            return False

        # Check for DX10 header
        header_size = 128
        fourcc = struct.unpack_from('<I', dds_data, 84)[0]
        if fourcc == 0x30315844:  # "DX10"
            header_size = 148

        payload = dds_data[header_size:]

        # Write cache file
        cache_dir.mkdir(parents=True, exist_ok=True)
        with open(cache_file, 'wb') as f:
            f.write(struct.pack('<III', target_w, target_h, len(payload)))
            f.write(payload)

        print(f"  OK: {logo_url} -> {cache_name} ({target_w}x{target_h}, {len(payload)} bytes)")
        return True


def main():
    parser = argparse.ArgumentParser(description="Pre-warm BC7 cache for station logos")
    parser.add_argument(
        "--data-dir",
        default=r"S:\SteamLibrary\steamapps\common\ForzaHorizon6\mods\frdx",
        help="Path to frdx data directory (contains stations.json and logos/)",
    )
    parser.add_argument(
        "--texconv",
        default=r"S:\SteamLibrary\steamapps\common\ForzaHorizon6\mods\frdx\bin\texconv.exe",
        help="Path to texconv.exe",
    )
    parser.add_argument("--width", type=int, default=196)
    parser.add_argument("--height", type=int, default=104)
    args = parser.parse_args()

    data_dir = Path(args.data_dir)
    texconv = Path(args.texconv)
    stations_path = data_dir / "stations.json"
    cache_dir = data_dir / "artwork_cache"

    if not stations_path.exists():
        print(f"stations.json not found: {stations_path}")
        sys.exit(1)
    if not texconv.exists():
        print(f"texconv not found: {texconv}")
        sys.exit(1)

    logo_urls = collect_logo_urls(stations_path)
    if not logo_urls:
        print("No local logos found in stations.json")
        sys.exit(0)

    print(f"Processing {len(logo_urls)} logo(s) at {args.width}x{args.height}...")
    ok = 0
    for url in logo_urls:
        if process_logo(url, data_dir, texconv, cache_dir, args.width, args.height):
            ok += 1

    print(f"\nDone: {ok}/{len(logo_urls)} cached -> {cache_dir}")


if __name__ == "__main__":
    main()
