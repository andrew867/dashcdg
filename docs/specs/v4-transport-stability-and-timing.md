# V4 transport stability under load (timing, jitter, PTP)

## Observed symptoms

- **Glitchy audio** when the host is busy: disk I/O, compilation, foreground IDE, web browsing.
- **Pause/unpause** sometimes recovers after multiple cycles; failures can return under load.
- **Legacy RX** (e.g. Pentium III) is expected to stay **low CPU** and **in sync**; modern TX is expected on **HPET/QPC-capable** systems.

## What is already true

- **Wall clock**: On Windows, `dashcdg_clock_now_ms()` uses **`QueryPerformanceCounter`** (QPC), which is **sub-millisecond** relative to the QPC frequency, not calendar drift.
- **Media clock**: Sender time on packets and PTP-style exchanges feed `dashcdg_media_clock_*` with **clamped** steps to avoid wild jumps.
- **Jitter buffer**: Fixed **slot count** (64 audio frames); drain uses **sender playback** and **late_grace** to skip missing frames instead of growing buffers indefinitely.

## Root causes (no buffer-size band-aids)

1. **Scheduling jitter**: Any non-real-time OS can delay **audio producer**, **network send**, or **receive** threads. Larger UDP buffers only **hide** loss until they overflow; they do not fix **thread latency**.
2. **Stale pipeline state** after stress: **jitter** + **FEC group** state can hold **pre-switch** frames or parity; **codec mismatch** without a matching **session_info** leaves decoders wrong (see [`v4-codec-switching-contract.md`](v4-codec-switching-contract.md)).
3. **PTP over UDP** inherits **same scheduling** as media; “hardware sub-ms PTP” on a general-purpose PC still **samples** time in software. Improvements are: **consistent thread priority**, **fewer contended locks** on hot paths, **immediate session_info** on codec change, and **receiver-side reconciliation**.

## Design rules

| Rule | Rationale |
| --- | --- |
| Do **not** increase default ring sizes as the primary fix | Masks symptoms; increases latency. |
| **Clear** jitter + FEC trackers on **codec reconcile** and on **configure_audio** | Prevents wedged sequences. |
| **Reconcile codec from `v4_audio_chunk`** when it disagrees with announced | Repairs lost **session_info** without restart. |
| TX sends **session_info immediately** on codec hotkey | Minimizes race with first new frames. |

## Optional platform tuning (documentation only unless product asks)

- Windows: **multimedia class scheduler** / **MMCSS** registration for the process (raises scheduling guarantee for audio threads).
- Raise **priority** of TX/RX **network/audio** threads cautiously (can starve UI).
- Ensure **power plan** is **High performance** on show machines to reduce CPU frequency scaling jitter.

## FEC and loss

Xor-based **FEC parity** for audio/CDG is separate datagrams (`DASHCDG_PACKET_FEC_PARITY`). Under burst loss, **parity in a following packet** is the standard pattern; **piggyback** parity inside **`v4_audio_chunk`** is a possible **future** wire revision (see [`v4-audio-fec-advanced.md`](v4-audio-fec-advanced.md)).

## Related documents

- [`v4-codec-switching-contract.md`](v4-codec-switching-contract.md)
- [`v4-network-stats-and-adaptation.md`](v4-network-stats-and-adaptation.md)
- [`../test/v4-transport-reliability-validation.md`](../test/v4-transport-reliability-validation.md)
