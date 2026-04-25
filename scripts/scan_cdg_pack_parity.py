#!/usr/bin/env python3
"""
Scan CDG byte streams for non-zero R-W PACK parity fields (IEC 60908 sense).

Layout matches C struct dashcdg_subchannel_packet (see core/include/dashcdg/cdg.h):
  bytes 0-1   command, instruction
  bytes 2-3   parity_q[2]
  bytes 4-19  data[16]
  bytes 20-23 parity_p[4]

Each on-disk CDG packet is 24 bytes. "Has parity data" means at least one packet has
any of the six parity bytes non-zero (typical software rips zero these bytes).

Usage:
  python scripts/scan_cdg_pack_parity.py
  python scripts/scan_cdg_pack_parity.py --roots cdg docs/specs
"""

import argparse
import sys
import zipfile
from pathlib import Path
from typing import Optional, Tuple

PACK = 24
PARITY_INDEXES = (2, 3, 20, 21, 22, 23)


def scan_stream(data: bytes) -> Optional[Tuple[int, int]]:
    if len(data) < PACK or len(data) % PACK != 0:
        return None
    n = len(data) // PACK
    nz = 0
    for i in range(n):
        pkt = data[i * PACK : (i + 1) * PACK]
        if any(pkt[j] != 0 for j in PARITY_INDEXES):
            nz += 1
    return n, nz


def _rel(repo: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(repo).as_posix()
    except ValueError:
        return path.as_posix()


def iter_cdg_streams(repo: Path, roots):
    for root in roots:
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*.cdg")):
            yield _rel(repo, p), p.read_bytes()
        for zp in sorted(root.rglob("*.zip")):
            try:
                zf = zipfile.ZipFile(zp)
            except zipfile.BadZipFile:
                continue
            with zf:
                for info in sorted(zf.infolist(), key=lambda i: i.filename):
                    if info.is_dir() or not info.filename.lower().endswith(".cdg"):
                        continue
                    inner = info.filename.replace("\\", "/")
                    yield f"{_rel(repo, zp)}::{inner}", zf.read(info.filename)


def main() -> int:
    ap = argparse.ArgumentParser(description="List CDG files with non-zero PACK parity bytes.")
    ap.add_argument(
        "--roots",
        nargs="*",
        default=["cdg", "docs/specs"],
        help="Directories to scan recursively (default: cdg docs/specs)",
    )
    ap.add_argument(
        "--markdown",
        action="store_true",
        help="Print a markdown table (with_parity only) to stdout",
    )
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    roots = [(repo / r).resolve() for r in args.roots]

    rows = []
    bad = []
    for label, data in iter_cdg_streams(repo, roots):
        st = scan_stream(data)
        if st is None:
            bad.append(label)
            continue
        n, nz = st
        rows.append((label, n, nz))

    with_parity = [r for r in rows if r[2] > 0]
    all_zero = [r for r in rows if r[2] == 0]

    if args.markdown:
        print("| Path | Packets (24 B) | Packets with any non-zero Q/P | % non-zero |")
        print("| --- | ---: | ---: | ---: |")
        for label, n, nz in sorted(with_parity, key=lambda x: x[0].lower()):
            pct = 100.0 * nz / n if n else 0.0
            print(f"| `{label}` | {n} | {nz} | {pct:.1f} |")
        return 0

    root_labels = ", ".join(_rel(repo, r) for r in roots)
    print(f"scanned {len(rows)} CDG streams under: {root_labels}")
    if bad:
        print(f"warning: {len(bad)} paths not multiple of {PACK} bytes (skipped)", file=sys.stderr)
    print(f"with_nonzero_parity: {len(with_parity)}")
    print(f"all_parity_bytes_zero: {len(all_zero)}")
    print()
    print("--- with non-zero PACK parity (Q or P) in at least one packet ---")
    for label, n, nz in sorted(with_parity, key=lambda x: x[0].lower()):
        pct = 100.0 * nz / n if n else 0.0
        print(f"{label}\tpackets={n}\tnz_parity_packets={nz}\t({pct:.1f}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
