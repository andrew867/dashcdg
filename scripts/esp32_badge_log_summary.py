#!/usr/bin/env python3
"""
Summarize ESP32 badge UART logs into quick reliability metrics.
"""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path


PATTERNS = {
    "startup_gate_warn": re.compile(r"audio startup gate:", re.IGNORECASE),
    "audio_no_dac_push_warn": re.compile(r"audio packets arriving but no DAC push", re.IGNORECASE),
    "wdt_panic": re.compile(r"Interrupt wdt timeout on CPU0", re.IGNORECASE),
    "stack_overflow": re.compile(r"stack overflow in task sys_evt", re.IGNORECASE),
    "select_enomem": re.compile(r"select:\s*ENOMEM", re.IGNORECASE),
    "igmp_enomem": re.compile(r"IGMP re-join ENOMEM", re.IGNORECASE),
    "dac_nomem": re.compile(r"dac_continuous.*NO_MEM|ESP_ERR_NO_MEM", re.IGNORECASE),
    "anchor_asm_malloc_fail": re.compile(r"v4 anchor asm malloc .* failed", re.IGNORECASE),
    "audio_dac_begin_fail": re.compile(r"v4_audio_dac_begin_fail", re.IGNORECASE),
    "ucast_media_open": re.compile(r"unicast RX dup media port", re.IGNORECASE),
    "wifi_ps0": re.compile(r"wifi:Set ps type:\s*0", re.IGNORECASE),
    "wifi_ps1": re.compile(r"wifi:pm start, type:\s*1", re.IGNORECASE),
    "rx_listening": re.compile(r"badge_rx:\s+listening UDP", re.IGNORECASE),
    "karaoke_ui_up": re.compile(r"karaoke UI up", re.IGNORECASE),
    "rx_stats_sent_log": re.compile(r"v4 rx-stats sendto", re.IGNORECASE),
}


def summarize_file(path: Path) -> dict[str, int | str]:
    counters: dict[str, int | str] = {"file": str(path)}
    for key in PATTERNS:
        counters[key] = 0
    counters["line_count"] = 0

    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            counters["line_count"] = int(counters["line_count"]) + 1
            for key, pattern in PATTERNS.items():
                if pattern.search(line):
                    counters[key] = int(counters[key]) + 1
    return counters


def print_table(rows: list[dict[str, int | str]]) -> None:
    cols = [
        "file",
        "line_count",
        "startup_gate_warn",
        "audio_no_dac_push_warn",
        "wdt_panic",
        "stack_overflow",
        "select_enomem",
        "igmp_enomem",
        "dac_nomem",
        "anchor_asm_malloc_fail",
        "ucast_media_open",
        "wifi_ps0",
        "wifi_ps1",
        "rx_listening",
        "karaoke_ui_up",
    ]
    widths = {c: len(c) for c in cols}
    for row in rows:
        for c in cols:
            widths[c] = max(widths[c], len(str(row.get(c, ""))))

    def fmt(row: dict[str, int | str]) -> str:
        return " | ".join(str(row.get(c, "")).ljust(widths[c]) for c in cols)

    print(fmt({c: c for c in cols}))
    print("-+-".join("-" * widths[c] for c in cols))
    for row in rows:
        print(fmt(row))


def write_csv(path: Path, rows: list[dict[str, int | str]]) -> None:
    if not rows:
        return
    fieldnames = list(rows[0].keys())
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    default_dir = repo_root / "docs" / "ops" / "logs" / "esp32-debug-cycle"
    p = argparse.ArgumentParser(description="Summarize one or more ESP32 UART log files.")
    p.add_argument("paths", nargs="*", help="Log file paths or directories (defaults to debug-cycle dir).")
    p.add_argument("--glob", default="*.log", help="Glob used when a directory is provided.")
    p.add_argument("--csv-out", default="", help="Optional CSV output file path.")
    return p.parse_args(), default_dir


def main() -> int:
    args, default_dir = parse_args()
    inputs = [Path(p) for p in args.paths] if args.paths else [default_dir]
    files: list[Path] = []
    for p in inputs:
        if p.is_dir():
            files.extend(sorted(p.glob(args.glob)))
        elif p.is_file():
            files.append(p)

    files = [f for f in files if f.suffix.lower() in (".log", ".txt")]
    if not files:
        print("No log files found.")
        return 1

    rows = [summarize_file(f) for f in files]
    print_table(rows)
    if args.csv_out:
        out = Path(args.csv_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        write_csv(out, rows)
        print(f"\nWrote CSV: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
