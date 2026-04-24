# Bad-network transport — implementation phases (companion)

## Purpose

Companion to **[bad-network-transport.md](bad-network-transport.md)**. That document defines goals, packet families, and success targets. This document orders **remaining implementation phases** and ties each phase to validation so redesign work stays traceable.

Execution slices for planned video repair windows are tracked in:
**[../ops/v4-video-repair-implementation-checklist.md](../ops/v4-video-repair-implementation-checklist.md)**.

## Phase map

| Phase | Scope | Primary validation |
| --- | --- | --- |
| **P0 — Baseline metrics** | Capture steady-state bitrate, burst bitrate, FEC overhead on Ethernet and on impaired relay ([desktop-impairment-validation.md](../test/desktop-impairment-validation.md)); document in soak archive. | [long-impairment-soak-validation.md](../test/long-impairment-soak-validation.md) |
| **P1 — Scheduler fairness** | Ensure no tick monopolizes the link (anchors, backfill, live media); align with success targets in bad-network-transport §Success Targets. | [bad-network-transport-validation.md](../test/bad-network-transport-validation.md) §2–3 |
| **P2 — Audio profiles** | `quality` vs `resilience` behavior per **[bad-network-audio-profiles.md](bad-network-audio-profiles.md)**; codec ids and redundancy fields in SESSION_INFO / announce path. | [bad-network-transport-validation.md](../test/bad-network-transport-validation.md), [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md) |
| **P3 — Repair strategy** | Move beyond single-payload XOR where spec requires (burst Wi-Fi); REPAIR_WINDOW / redundancy semantics per bad-network-transport §Repair. | Matrix §3–4 in bad-network-transport-validation; soak for long-run stability |
| **P3.5 — Interleaved video repair (embedded-first)** | Add bounded forward/reverse parity windows for `v4_video_delta` so one-miss-in-window recovery can happen on-the-fly without waiting for the next full anchor. Keep wire additive and backward compatible. | [v4-transport-reliability-validation.md](../test/v4-transport-reliability-validation.md), [embedded-hardware-bringup-validation.md](../test/embedded-hardware-bringup-validation.md), impairment soak |
| **P4 — Late join + loading path** | Loading screen, compact anchor, staged backfill — **[architecture/bad-network-startup-path.md](../architecture/bad-network-startup-path.md)**. | §1 Matrix in bad-network-transport-validation |
| **P5 — Documentation + wire freeze** | When phases meet targets, freeze “bad-network mode” wire subset for embedded/portable consumers. | Release checklist + ops gates |

## Relationship to audio profiles

All profile-specific behavior must remain consistent with:

- **[bad-network-audio-profiles.md](bad-network-audio-profiles.md)** — bitrate class, redundancy, RX expansion.
- **[narrowband-low-bitrate-audio-quality.md](narrowband-low-bitrate-audio-quality.md)** — subjective and objective quality bars when narrowband/resilience paths change.

## Exit criteria (phase complete)

A phase may merge when:

1. Linked validation sections pass on at least **two** desktop variants (e.g. x64 GL + x86 GDI or retro subset where applicable).
2. No known **silent wedge** (audio never starts while video progresses) on late join for that phase’s features.
3. Docs updated: bad-network-transport or this file — **no orphan behavior**.
