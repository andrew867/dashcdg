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
import string
import sys
import zipfile
from pathlib import Path
from typing import Optional, Tuple

PACK = 24
PARITY_INDEXES = (2, 3, 20, 21, 22, 23)
RS_MOD = 63
RS_ALOG = [
    1, 2, 4, 8, 16, 32, 3, 6, 12, 24, 48, 35, 5, 10, 20, 40, 19, 38, 15, 30, 60, 59, 53, 41, 17, 34, 7, 14, 28,
    56, 51, 37, 9, 18, 36, 11, 22, 44, 27, 54, 47, 29, 58, 55, 45, 25, 50, 39, 13, 26, 52, 43, 21, 42, 23, 46, 31,
    62, 63, 61, 57, 49, 33, 0,
]
RS_LOG = [
    0, 0, 1, 6, 2, 12, 7, 26, 3, 32, 13, 35, 8, 48, 27, 18, 4, 24, 33, 16, 14, 52, 36, 54, 9, 45, 49, 38, 28, 41,
    19, 56, 5, 62, 25, 11, 34, 31, 17, 47, 15, 23, 53, 51, 37, 44, 55, 40, 10, 61, 46, 30, 50, 22, 39, 43, 29, 60,
    42, 21, 20, 59, 57, 58,
]


def _gf_nonzero(data: int, shift: int) -> int:
    return RS_ALOG[(RS_LOG[data] + shift) % RS_MOD]


def pack_rs_syndrome_ok(pkt: bytes) -> bool:
    q0 = q1 = 0
    p0 = p1 = p2 = p3 = 0
    p24 = [b & 0x3F for b in pkt]
    for i in range(3, -1, -1):
        data = p24[3 - i]
        if data:
            q0 ^= _gf_nonzero(data, i * 0)
            q1 ^= _gf_nonzero(data, i * 1)
    if q0 or q1:
        return False
    for i in range(23, -1, -1):
        data = p24[23 - i]
        if data:
            p0 ^= _gf_nonzero(data, i * 0)
            p1 ^= _gf_nonzero(data, i * 1)
            p2 ^= _gf_nonzero(data, i * 2)
            p3 ^= _gf_nonzero(data, i * 3)
    return not (p0 or p1 or p2 or p3)


def _ascii_view(data: bytes) -> str:
    allowed = set(string.printable.encode("ascii"))
    return "".join(chr(b) if b in allowed and chr(b) not in "\r\n\t\x0b\x0c" else "." for b in data)


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
    ap.add_argument(
        "--dump-rs-fails",
        type=str,
        default="",
        help="Optional output text file: dump packets whose non-zero parity fails RS syndrome",
    )
    ap.add_argument(
        "--dump-limit",
        type=int,
        default=2000,
        help="Max failing packets to dump (default: 2000)",
    )
    ap.add_argument(
        "--only",
        type=str,
        default="",
        help="Only process labels containing this case-insensitive substring",
    )
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    roots = [(repo / r).resolve() for r in args.roots]

    rows = []
    bad = []
    dumped = 0
    dump_handle = None
    only_sub = args.only.lower().strip()
    if args.dump_rs_fails:
        dump_path = (repo / args.dump_rs_fails).resolve() if not Path(args.dump_rs_fails).is_absolute() else Path(args.dump_rs_fails)
        dump_path.parent.mkdir(parents=True, exist_ok=True)
        dump_handle = dump_path.open("w", encoding="utf-8")
        dump_handle.write("# CDG PACK parity RS fail dump\n")
    for label, data in iter_cdg_streams(repo, roots):
        if only_sub and only_sub not in label.lower():
            continue
        st = scan_stream(data)
        if st is None:
            bad.append(label)
            continue
        n, nz = st
        rows.append((label, n, nz))
        if dump_handle is not None and nz > 0 and dumped < args.dump_limit:
            for i in range(n):
                pkt = data[i * PACK : (i + 1) * PACK]
                if any(pkt[j] != 0 for j in PARITY_INDEXES) and not pack_rs_syndrome_ok(pkt):
                    q_hex = " ".join(f"{pkt[j]:02X}" for j in (2, 3))
                    p_hex = " ".join(f"{pkt[j]:02X}" for j in (20, 21, 22, 23))
                    dump_handle.write(
                        f"{label}\tpacket={i}\toffset={i*PACK}\tq=[{q_hex}]\tp=[{p_hex}]\n"
                    )
                    dump_handle.write(f"  pkt_hex={pkt.hex()}\n")
                    dump_handle.write(f"  pkt_ascii={_ascii_view(pkt)}\n")
                    dumped += 1
                    if dumped >= args.dump_limit:
                        break

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
    if dump_handle is not None:
        dump_handle.write(f"# dumped_fail_packets={dumped}\n")
        dump_handle.close()
        print()
        print(f"rs_fail_dump_packets={dumped}")
        print(f"rs_fail_dump_file={dump_path.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
