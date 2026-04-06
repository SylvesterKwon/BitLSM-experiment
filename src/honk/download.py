#!/usr/bin/env python3
"""NYC Yellow Taxi parquet data download script."""

import argparse
import os
import sys
from typing import List, Tuple

import requests

BASE_URL = "https://d37ci6vzurychx.cloudfront.net/trip-data"


def parse_years(year_args: List[str]) -> List[int]:
    """Parse year arguments. Supports range '2022-2024' or individual '2022 2023'."""
    years = []
    for arg in year_args:
        if "-" in arg:
            parts = arg.split("-")
            if len(parts) != 2:
                raise ValueError(f"Invalid year range: {arg}")
            start, end = int(parts[0]), int(parts[1])
            if start > end:
                raise ValueError(f"Invalid year range: {arg} (start > end)")
            years.extend(range(start, end + 1))
        else:
            years.append(int(arg))
    return sorted(set(years))


def build_file_list(years: List[int], months: List[int]) -> List[Tuple[str, str]]:
    """Build a list of (filename, url) tuples."""
    files = []
    for year in years:
        for month in months:
            filename = f"yellow_tripdata_{year}-{month:02d}.parquet"
            url = f"{BASE_URL}/{filename}"
            files.append((filename, url))
    return files


def get_remote_size(url: str) -> int | None:
    """Get file size (bytes) via HEAD request. Returns None on failure."""
    try:
        resp = requests.head(url, allow_redirects=True, timeout=10)
        if resp.status_code == 200 and "Content-Length" in resp.headers:
            return int(resp.headers["Content-Length"])
    except requests.RequestException:
        pass
    return None


def format_size(size_bytes: int) -> str:
    """Convert bytes to a human-readable size string."""
    if size_bytes < 1024 ** 2:
        return f"{size_bytes / 1024:.1f} KB"
    if size_bytes < 1024 ** 3:
        return f"{size_bytes / 1024 ** 2:.1f} MB"
    return f"{size_bytes / 1024 ** 3:.2f} GB"


def download_file(url: str, dest: str) -> None:
    """Download a file with streaming."""
    resp = requests.get(url, stream=True, timeout=30)
    resp.raise_for_status()
    total = int(resp.headers.get("Content-Length", 0))
    downloaded = 0

    with open(dest, "wb") as f:
        for chunk in resp.iter_content(chunk_size=8192):
            f.write(chunk)
            downloaded += len(chunk)
            if total > 0:
                pct = downloaded / total * 100
                print(f"\r  Downloading: {pct:.1f}%", end="", flush=True)
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Download NYC Yellow Taxi trip data (parquet)."
    )
    parser.add_argument(
        "--year", nargs="+", required=True,
        help="Year(s) to download. Supports range (2022-2024) or individual (2022 2023).",
    )
    parser.add_argument(
        "--month", nargs="+", type=int, default=list(range(1, 13)),
        help="Month(s) to download (default: 1-12).",
    )
    parser.add_argument(
        "--output-dir", default="./data",
        help="Directory to save parquet files (default: ./data).",
    )
    parser.add_argument(
        "-y", "--yes", action="store_true",
        help="Skip confirmation prompt.",
    )
    args = parser.parse_args()

    years = parse_years(args.year)
    months = args.month
    output_dir = args.output_dir
    os.makedirs(output_dir, exist_ok=True)

    file_list = build_file_list(years, months)

    # Filter out already existing files
    to_download = []
    skipped = 0
    for filename, url in file_list:
        dest = os.path.join(output_dir, filename)
        if os.path.exists(dest):
            skipped += 1
        else:
            to_download.append((filename, url))

    if not to_download:
        print(f"All {len(file_list)} files already exist. Nothing to download.")
        return

    if skipped > 0:
        print(f"Skipping {skipped} already downloaded file(s).\n")

    # Check sizes via HEAD requests
    print(f"Checking {len(to_download)} file(s)...\n")
    sizes = {}
    total_size = 0
    for filename, url in to_download:
        size = get_remote_size(url)
        sizes[filename] = size
        if size is not None:
            total_size += size

    # Print file list and sizes
    print(f"{'File':<45} {'Size':>10}")
    print("-" * 57)
    for filename, url in to_download:
        size = sizes[filename]
        size_str = format_size(size) if size is not None else "unknown"
        print(f"  {filename:<43} {size_str:>10}")
    print("-" * 57)
    print(f"  Total: {len(to_download)} file(s), {format_size(total_size)}")
    print()

    # User confirmation
    if not args.yes:
        answer = input("Proceed with download? [y/N] ").strip().lower()
        if answer != "y":
            print("Aborted.")
            sys.exit(0)

    # Download
    for i, (filename, url) in enumerate(to_download, 1):
        dest = os.path.join(output_dir, filename)
        print(f"[{i}/{len(to_download)}] {filename}")
        try:
            download_file(url, dest)
        except requests.RequestException as e:
            print(f"  Error: {e}", file=sys.stderr)
            # Remove partial file on failure
            if os.path.exists(dest):
                os.remove(dest)
            continue

    print("\nDone.")


if __name__ == "__main__":
    main()
