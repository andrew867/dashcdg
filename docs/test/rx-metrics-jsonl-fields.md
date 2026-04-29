# desktop-rx `rx_metrics` jsonl fields (operator reference)

Emission: `platform/desktop/src/app_rx.c` (`dashcdg_rx_metrics_emit_locked`).

- **`clock_skew_ema_ms`**: Disciplined media-clock offset in milliseconds (same family as v4 `clock_offset_estimate_ms` on the wire). This is **not** `header.sender_time_ms - local_now` (those values can use unrelated epoch bases and previously produced useless **±9999** clamps).
- **`packet_wall_delta_skew_ema_ms`**: EMA of **(Δsender_time_ms − Δlocal_now)** between **consecutive** datagrams. This isolates link scheduling and clock-rate mismatch (typically small ms), useful for spotting burstiness or TX stalls.
- **`cdg_lag_ms`**: `heard_playback_ms − cdg_batch_jitter.next_playback_ms` when the CDG jitter buffer is initialized. `next_playback_ms` tracks the **next** batch **release** boundary; it can sit **ahead** of heard audio by **roughly the end-to-end pipeline** (preroll + ring + host), so large negative values often reflect **expected buffering**, not a broken canvas.
- **`cdg_render_skew_ms`**: `heard_playback_ms − packet_index_to_ms(live_state.ts)` — closer to **on-screen** vs **heard** when `live_state.ts` tracks the rendered canvas.

`heard_playback_ms` in metrics uses the same **stable host output latency** adjustment as v4 `presented_audio_timestamp_ms` (see `dashcdg_rx_stable_host_adjust_presented_timestamp_locked`).
