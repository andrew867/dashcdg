# V5 multistream adaptation (architecture placeholder)

This document tracks **planned** protocol and implementation work for separating PTP, CDG, audio, and control onto distinct multicast destinations, simulcasting multiple audio codecs, client-side adaptation with closed-loop stats, and optional piggyback FEC (see `docs/specs/v4-audio-fec-advanced.md`). **V4 remains the on-wire default** until a versioned wire format is fully specified and implemented.

## Enterprise timing (implemented on Windows desktop)

The desktop TX/RX apps call `timeBeginPeriod(1)` once per process and register streaming worker threads with MMCSS **Pro Audio** (`AvSetMmThreadCharacteristicsW` / `AvSetMmThreadPriority`), with fallbacks to raised thread priority. Source: `platform/desktop/src/win32_timing_boost.c`, wired from `app_tx.c` / `app_rx.c`. PE import tables include **`AVRT.dll`** and **`WINMM.dll`** on linked EXEs (system components).

Goals:

- Reduce `Sleep()` quantization under foreground IDE / host load (not “glitch-free under arbitrary load”).
- Give audio/network/PTP threads a fairer shot at CPU scheduling next to UI-heavy processes.

Non-Windows builds compile stubs (no-op).

## Future V5 topics (not implemented here)

- Snapshot + delta catch-up for late join and after pause/resume; keyframe / repair-window tuning tied to PTP.
- Separate multicast groups or ports per logical stream; optional multi-codec simulcast from TX with client group selection.
- Bandwidth ladder: Opus VBR → PCM rate/width → lower-rate codecs; client feedback to TX.
- Versioned payloads for piggyback / “next FEC in previous audio packet” and related tradeoffs.

When the wire format is ready, bump announced transport version to `DASHCDG_PROTOCOL_VERSION_V5` and keep V4 available for compatibility and testing.
