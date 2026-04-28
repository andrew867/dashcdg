#!/usr/bin/env python3
import argparse
import json
import math
import statistics
from collections import defaultdict


def percentile(values, p):
    if not values:
        return None
    data = sorted(values)
    if len(data) == 1:
        return float(data[0])
    rank = (len(data) - 1) * p
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return float(data[lo])
    frac = rank - lo
    return float(data[lo] * (1.0 - frac) + data[hi] * frac)


def summarize_series(name, values):
    if not values:
        return f"{name}: n=0"
    p50 = percentile(values, 0.50)
    p95 = percentile(values, 0.95)
    p99 = percentile(values, 0.99)
    mean = statistics.fmean(values)
    return (
        f"{name}: n={len(values)} mean={mean:.2f} "
        f"p50={p50:.2f} p95={p95:.2f} p99={p99:.2f} "
        f"min={min(values):.2f} max={max(values):.2f}"
    )


def load_records(paths):
    records = []
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(obj, dict) and obj.get("type") in {"tx_metrics", "rx_metrics"}:
                    obj["_src"] = path
                    records.append(obj)
    return records


def main():
    parser = argparse.ArgumentParser(
        description="Summarize dashcdg TX/RX sync metrics JSONL into soak verdicts."
    )
    parser.add_argument("jsonl", nargs="+", help="One or more metrics JSONL files.")
    parser.add_argument(
        "--same-backend",
        action="store_true",
        help="Apply same-backend gates (10/20 ms p95/p99) instead of mixed (20/40).",
    )
    args = parser.parse_args()

    records = load_records(args.jsonl)
    if not records:
        print("No tx_metrics/rx_metrics records found.")
        return 1

    tx = [r for r in records if r["type"] == "tx_metrics"]
    rx = [r for r in records if r["type"] == "rx_metrics"]
    by_rx = defaultdict(list)
    for r in rx:
        by_rx[r.get("receiver_instance", 0)].append(r)

    phase_spread = [float(r.get("phase_spread_ms", 0.0)) for r in tx]
    phase_warn_count = sum(1 for r in records if int(r.get("phase_warn", 0)) != 0)
    phase_fail_count = sum(1 for r in records if int(r.get("phase_fail", 0)) != 0)
    clock_noisy_count = sum(1 for r in records if int(r.get("clock_noisy", 0)) != 0)

    thresh_p95 = 10.0 if args.same_backend else 20.0
    thresh_p99 = 20.0 if args.same_backend else 40.0
    p95 = percentile(phase_spread, 0.95) if phase_spread else None
    p99 = percentile(phase_spread, 0.99) if phase_spread else None
    pass_p95 = p95 is not None and p95 <= thresh_p95
    pass_p99 = p99 is not None and p99 <= thresh_p99

    print("== dashcdg sync metrics summary ==")
    print(f"records: total={len(records)} tx={len(tx)} rx={len(rx)} receivers={len(by_rx)}")
    print(summarize_series("tx.phase_spread_ms", phase_spread))
    print(f"flags: phase_warn={phase_warn_count} phase_fail={phase_fail_count} clock_noisy={clock_noisy_count}")
    print(
        "gate: "
        f"p95<={thresh_p95:.0f} ({'PASS' if pass_p95 else 'FAIL'}) "
        f"p99<={thresh_p99:.0f} ({'PASS' if pass_p99 else 'FAIL'})"
    )
    print("")

    for receiver_id, samples in sorted(by_rx.items(), key=lambda kv: kv[0]):
        buf_ms = [float(s.get("audio_buffer_ms", 0.0)) for s in samples]
        host_ms = [float(s.get("host_latency_ms", 0.0)) for s in samples]
        trim_ppm = [float(s.get("trim_ppm", 0.0)) for s in samples]
        clock_off_ms = [float(s.get("clock_offset_estimate_ms", 0.0)) for s in samples]
        ptp_off_us = [float(s.get("ptp_offset_ema_us", 0.0)) for s in samples]
        rec_host = max(int(s.get("recover_host_underrun", 0)) for s in samples)
        rec_zero = max(int(s.get("recover_zero_buffer", 0)) for s in samples)
        rec_silent = max(int(s.get("recover_silent_stall", 0)) for s in samples)

        print(f"-- receiver {receiver_id} --")
        print(summarize_series("audio_buffer_ms", buf_ms))
        print(summarize_series("host_latency_ms", host_ms))
        print(summarize_series("trim_ppm", trim_ppm))
        print(summarize_series("clock_offset_estimate_ms", clock_off_ms))
        print(summarize_series("ptp_offset_ema_us", ptp_off_us))
        print(
            f"recovery_counters: host_underrun={rec_host} "
            f"zero_buffer={rec_zero} silent_stall={rec_silent}"
        )
        print("")

    overall_pass = pass_p95 and pass_p99 and phase_fail_count == 0
    print(f"overall_verdict: {'PASS' if overall_pass else 'FAIL'}")
    return 0 if overall_pass else 2


if __name__ == "__main__":
    raise SystemExit(main())
