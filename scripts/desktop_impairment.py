#!/usr/bin/env python3
"""Relay multicast UDP with repeatable impairments for desktop proof runs."""

from __future__ import annotations

import argparse
import random
import select
import socket
import struct
import sys
import time
from dataclasses import dataclass
from typing import Optional


@dataclass
class Counters:
    received: int = 0
    forwarded: int = 0
    dropped: int = 0
    reordered: int = 0
    burst_dropped: int = 0
    bytes_received: int = 0
    bytes_forwarded: int = 0
    pending_flushes: int = 0
    throttle_events: int = 0
    throttle_sleep_ms: int = 0


@dataclass
class PendingPacket:
    payload: bytes
    release_at: float


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be > 0")
    return parsed


def non_negative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return parsed


def percent_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0.0 or parsed > 100.0:
        raise argparse.ArgumentTypeError("must be between 0 and 100")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    examples = """Examples:
  python scripts/desktop_impairment.py --listen-group 239.255.77.91 --listen-port 24684 --emit-group 239.255.77.92 --emit-port 24685
  python scripts/desktop_impairment.py --listen-group 239.255.77.91 --listen-port 24684 --emit-group 239.255.77.92 --emit-port 24685 --drop-every 7
  python scripts/desktop_impairment.py --listen-group 239.255.77.91 --listen-port 24684 --emit-group 239.255.77.92 --emit-port 24685 --drop-percent 5 --seed 42
  python scripts/desktop_impairment.py --listen-group 239.255.77.91 --listen-port 24684 --emit-group 239.255.77.92 --emit-port 24685 --reorder-every 9 --reorder-hold-ms 80
  python scripts/desktop_impairment.py --listen-group 239.255.77.91 --listen-port 24684 --emit-group 239.255.77.92 --emit-port 24685 --burst-every 25 --burst-length 3
  python scripts/desktop_impairment.py --listen-group 239.255.77.91 --listen-port 24684 --emit-group 239.255.77.92 --emit-port 24685 --drop-every 6 --reorder-every 11 --max-packets 200
  python scripts/desktop_impairment.py --listen-group 239.255.77.91 --listen-port 24684 --emit-group 239.255.77.92 --emit-port 24685 --max-bytes-per-second 112500
"""
    parser = argparse.ArgumentParser(
        description="Relay multicast UDP with deterministic loss, reorder, and burst-loss impairments.",
        epilog=examples,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--listen-group", required=True, help="Input multicast group to join.")
    parser.add_argument("--listen-port", required=True, type=positive_int, help="Input multicast UDP port.")
    parser.add_argument("--emit-group", required=True, help="Output multicast group to send impaired traffic to.")
    parser.add_argument("--emit-port", required=True, type=positive_int, help="Output multicast UDP port.")
    parser.add_argument("--drop-every", type=positive_int, default=0, help="Drop every Nth received packet.")
    parser.add_argument("--drop-percent", type=percent_float, default=0.0, help="Random drop percentage from 0 to 100.")
    parser.add_argument("--burst-every", type=positive_int, default=0, help="Start a burst drop on every Nth received packet.")
    parser.add_argument("--burst-length", type=positive_int, default=2, help="Packets to drop once a burst starts. Default: 2.")
    parser.add_argument("--reorder-every", type=positive_int, default=0, help="Hold every Nth accepted packet and release it after the next accepted packet.")
    parser.add_argument("--reorder-hold-ms", type=non_negative_int, default=80, help="Flush held reorder packet after this many ms if no later packet arrives. Default: 80.")
    parser.add_argument("--ttl", type=positive_int, default=1, help="Multicast TTL for emitted packets. Default: 1.")
    parser.add_argument(
        "--max-bytes-per-second",
        type=non_negative_int,
        default=0,
        help="Throttle emitted traffic to this many bytes/sec. Default: unlimited.",
    )
    parser.add_argument("--max-packets", type=positive_int, default=0, help="Stop after this many received packets. Default: run until interrupted.")
    parser.add_argument("--stats-interval-ms", type=positive_int, default=1000, help="Print stats every N ms. Default: 1000.")
    parser.add_argument("--seed", type=int, default=0, help="Seed for repeatable random drop decisions. Default: 0.")
    parser.add_argument("--dry-run", action="store_true", help="Print the resolved config and exit without opening sockets.")
    return parser


def create_input_socket(group: str, port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", port))
    membership = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton("0.0.0.0"))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
    sock.setblocking(False)
    return sock


def create_output_socket(ttl: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, ttl)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    return sock


def print_config(args: argparse.Namespace) -> None:
    print(
        "config:"
        f" listen={args.listen_group}:{args.listen_port}"
        f" emit={args.emit_group}:{args.emit_port}"
        f" drop_every={args.drop_every}"
        f" drop_percent={args.drop_percent:.2f}"
        f" burst_every={args.burst_every}"
        f" burst_length={args.burst_length}"
        f" reorder_every={args.reorder_every}"
        f" reorder_hold_ms={args.reorder_hold_ms}"
        f" ttl={args.ttl}"
        f" max_bytes_per_second={args.max_bytes_per_second}"
        f" max_packets={args.max_packets}"
        f" seed={args.seed}"
    )


def emit_packet(
    sock: socket.socket,
    destination: tuple[str, int],
    payload: bytes,
    counters: Counters,
    max_bytes_per_second: int,
    next_send_time: float,
) -> float:
    if max_bytes_per_second > 0:
        now = time.monotonic()
        send_at = max(now, next_send_time)
        if send_at > now:
            sleep_seconds = send_at - now
            time.sleep(sleep_seconds)
            counters.throttle_events += 1
            counters.throttle_sleep_ms += int(sleep_seconds * 1000.0)

    sent = sock.sendto(payload, destination)
    if sent != len(payload):
        raise RuntimeError(f"short send: sent {sent} of {len(payload)} bytes")
    counters.forwarded += 1
    counters.bytes_forwarded += sent
    if max_bytes_per_second > 0:
        return max(time.monotonic(), next_send_time) + (sent / float(max_bytes_per_second))
    return next_send_time


def maybe_flush_pending(
    output_sock: socket.socket,
    destination: tuple[str, int],
    pending: Optional[PendingPacket],
    counters: Counters,
    now: float,
    max_bytes_per_second: int,
    next_send_time: float,
) -> tuple[Optional[PendingPacket], float]:
    if pending is not None and now >= pending.release_at:
        next_send_time = emit_packet(
            output_sock,
            destination,
            pending.payload,
            counters,
            max_bytes_per_second,
            next_send_time,
        )
        counters.pending_flushes += 1
        return None, next_send_time
    return pending, next_send_time


def print_stats(counters: Counters, started_at: float) -> None:
    elapsed_ms = int((time.monotonic() - started_at) * 1000.0)
    print(
        "stats:"
        f" received={counters.received}"
        f" forwarded={counters.forwarded}"
        f" dropped={counters.dropped}"
        f" burst_dropped={counters.burst_dropped}"
        f" reordered={counters.reordered}"
        f" pending_flushes={counters.pending_flushes}"
        f" throttle_events={counters.throttle_events}"
        f" throttle_sleep_ms={counters.throttle_sleep_ms}"
        f" bytes_in={counters.bytes_received}"
        f" bytes_out={counters.bytes_forwarded}"
        f" elapsed_ms={elapsed_ms}"
    )
    sys.stdout.flush()


def run(args: argparse.Namespace) -> int:
    rng = random.Random(args.seed)
    burst_remaining = 0
    counters = Counters()
    pending: Optional[PendingPacket] = None
    destination = (args.emit_group, args.emit_port)
    started_at = time.monotonic()
    last_report_at = started_at
    next_send_time = started_at

    print_config(args)
    if args.dry_run:
        print("dry_run: no sockets opened")
        return 0

    input_sock = create_input_socket(args.listen_group, args.listen_port)
    output_sock = create_output_socket(args.ttl)
    print("relay: started")
    sys.stdout.flush()

    try:
        while True:
            now = time.monotonic()
            pending, next_send_time = maybe_flush_pending(
                output_sock,
                destination,
                pending,
                counters,
                now,
                args.max_bytes_per_second,
                next_send_time,
            )

            if args.max_packets and counters.received >= args.max_packets:
                break

            timeout = max(0.0, (args.stats_interval_ms / 1000.0) - (now - last_report_at))
            if pending is not None:
                timeout = min(timeout, max(0.0, pending.release_at - now))
            readable, _, _ = select.select([input_sock], [], [], timeout)
            now = time.monotonic()

            if now - last_report_at >= args.stats_interval_ms / 1000.0:
                print_stats(counters, started_at)
                last_report_at = now

            if not readable:
                continue

            payload, _ = input_sock.recvfrom(65535)
            counters.received += 1
            counters.bytes_received += len(payload)
            packet_index = counters.received

            drop_packet = False
            if burst_remaining > 0:
                drop_packet = True
                burst_remaining -= 1
                counters.burst_dropped += 1
            elif args.burst_every and packet_index % args.burst_every == 0:
                drop_packet = True
                burst_remaining = max(0, args.burst_length - 1)
                counters.burst_dropped += 1
            elif args.drop_every and packet_index % args.drop_every == 0:
                drop_packet = True
            elif args.drop_percent > 0.0 and rng.random() < args.drop_percent / 100.0:
                drop_packet = True

            if drop_packet:
                counters.dropped += 1
                continue

            if args.reorder_every and packet_index % args.reorder_every == 0 and pending is None:
                pending = PendingPacket(
                    payload=payload,
                    release_at=now + (args.reorder_hold_ms / 1000.0),
                )
                counters.reordered += 1
                continue

            next_send_time = emit_packet(
                output_sock,
                destination,
                payload,
                counters,
                args.max_bytes_per_second,
                next_send_time,
            )
            if pending is not None:
                next_send_time = emit_packet(
                    output_sock,
                    destination,
                    pending.payload,
                    counters,
                    args.max_bytes_per_second,
                    next_send_time,
                )
                pending = None

        pending, next_send_time = maybe_flush_pending(
            output_sock,
            destination,
            pending,
            counters,
            time.monotonic() + 3600.0,
            args.max_bytes_per_second,
            next_send_time,
        )
        if pending is not None:
            emit_packet(
                output_sock,
                destination,
                pending.payload,
                counters,
                args.max_bytes_per_second,
                next_send_time,
            )
        print("relay: finished")
        print_stats(counters, started_at)
        return 0
    finally:
        input_sock.close()
        output_sock.close()


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return run(args)
    except KeyboardInterrupt:
        print("relay: interrupted")
        return 130
    except OSError as exc:
        print(f"error: socket failure: {exc}", file=sys.stderr)
        return 1
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
