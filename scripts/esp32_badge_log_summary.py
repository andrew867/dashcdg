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
    # Video / CDG path (ESP_LOGW ring_full from badge_rx when jitter cannot accept a delta)
    "cdg_jitter_ring_full": re.compile(r"CDG jitter ring_full", re.IGNORECASE),
    "v4_anchor_applied_log": re.compile(r"v4 anchor applied", re.IGNORECASE),
    "rx_task_exit": re.compile(r"badge_rx:\s+rx task exit|rx task exit", re.IGNORECASE),
    "rx_stopped_log": re.compile(r"badge_rx:\s+rx stopped", re.IGNORECASE),
    "igmp_bootstrap_pass": re.compile(r"IGMP bootstrap:", re.IGNORECASE),
    "sta_got_ip": re.compile(r"got ip:|STA_GOT_IP|esp_netif_handlers:\s+Got IP", re.IGNORECASE),
    # badge_rx.c audio-only UART line: a_rx=chunks a_out=frames_to_dac jb=occ/cap ...
    "audio_only_uart_stats": re.compile(r"badge_rx:\s+audio_only\s+a_rx=", re.IGNORECASE),
    # Per-interval delta diagnostics (d_skip, d_udp, jb_pk, …) — compare logs before/after fixes
    "audio_chop_uart_stats": re.compile(r"badge_rx:\s+audio_chop\s+", re.IGNORECASE),
    # Audio-only cumulative line (mtx_idle_miss / lvgl_coop / rx_mtx_to)
    "audio_only_uart_mtx": re.compile(r"badge_rx:\s+audio_only\s+.*mtx_idle_miss=", re.IGNORECASE),
    # Hardware proof: DAC DMA chunks accepted vs ESP-IDF errors (grep AUDIO_UART_PROOF)
    "audio_uart_proof": re.compile(r"badge_rx:\s+AUDIO_UART_PROOF\s+", re.IGNORECASE),
    # ---- FreeRTOS executive refactor (T1..T9) ----
    # T1/T2 boot orchestrator: structured trace line emitted on each bit/health update.
    "exec_trace_line": re.compile(r"\[exec-trace\]", re.IGNORECASE),
    "exec_boot_publish": re.compile(r"\[exec-trace\]\s+boot_publish\s+", re.IGNORECASE),
    "exec_boot_complete_nominal": re.compile(r"\[exec-trace\]\s+boot_complete\s+.*nominal", re.IGNORECASE),
    "exec_boot_complete_degraded": re.compile(r"\[exec-trace\]\s+boot_complete\s+.*degraded", re.IGNORECASE),
    "exec_health_degraded": re.compile(r"\[exec-trace\]\s+health\s+.*=degraded", re.IGNORECASE),
    "exec_health_timeout": re.compile(r"\[exec-trace\]\s+health\s+.*=timeout", re.IGNORECASE),
    # T2 boot DHCP timer expiry (Wi-Fi STA never got IP within boot window).
    "exec_wifi_dhcp_timeout": re.compile(r"BOOT_WIFI_DHCP_TIMEOUT|wifi_dhcp_timeout", re.IGNORECASE),
    # T7 liveness sweep: stall observation lines + enforce transitions.
    "exec_liveness_stall": re.compile(r"\[exec-trace\]\s+liveness_stall\s+", re.IGNORECASE),
    "exec_liveness_enforce": re.compile(r"\[exec-trace\]\s+liveness_enforce\s+", re.IGNORECASE),
    # T8 LVGL tick budget: throttled overrun line per tick name.
    "exec_ui_tick_over": re.compile(r"\[exec-trace\]\s+ui_tick_over\s+|ui-tick over budget", re.IGNORECASE),
    # T9 DAC route arbitration degraded transitions.
    "exec_dac_degraded": re.compile(r"\[exec-trace\]\s+dac_degraded\s+", re.IGNORECASE),
    # T5/T6 hot-path bounded-wait drops: surfaced via badge_rx_get_stats but also appears as ESP_LOGW.
    "rx_mtx_timeout_pump": re.compile(r"rx_mtx_pump_timeouts|s_mtx pump timeout", re.IGNORECASE),
    "rx_mtx_timeout_repair": re.compile(r"rx_mtx_repair_timeouts|s_mtx repair timeout", re.IGNORECASE),
    # T4 RX command queue back-pressure (commands dropped when q full).
    "rx_cmd_q_drop": re.compile(r"rx cmd q full:", re.IGNORECASE),
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
        "cdg_jitter_ring_full",
        "v4_anchor_applied_log",
        "rx_task_exit",
        "sta_got_ip",
        "audio_only_uart_stats",
        "audio_chop_uart_stats",
        "audio_only_uart_mtx",
        "audio_uart_proof",
        "exec_trace_line",
        "exec_boot_publish",
        "exec_boot_complete_nominal",
        "exec_boot_complete_degraded",
        "exec_health_degraded",
        "exec_health_timeout",
        "exec_wifi_dhcp_timeout",
        "exec_liveness_stall",
        "exec_liveness_enforce",
        "exec_ui_tick_over",
        "exec_dac_degraded",
        "rx_mtx_timeout_pump",
        "rx_mtx_timeout_repair",
        "rx_cmd_q_drop",
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
