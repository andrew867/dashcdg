# RCA: Desktop RX audio pipeline (v4 → jitter → decode → PortAudio)

**Scope:** `platform/desktop/src/app_rx.c`, `platform/desktop/src/desktop_audio.c`, `core/src/audio_jitter.c`. **Not** ncurses / UI libraries on RX (no Makefile change for stub objects).

**Related docs:**  
- `docs/specs/desktop-rx-p3-gdi-audio-stall-rca.md` — long-run stall, starvation gate, `Pa_IsStreamActive`, buffered-silent timestamp noise.  
- `docs/specs/desktop-tx-audio-pipeline-playlist-advance-rca.md` — TX encoder seek / session bootstrap; RX behavior depends on correct first-frame tags from TX.

---

## 1. Architecture (two directions)

### 1.1 Network → PCM ring (“in” toward the speaker)

1. **UDP / parse** (`dashcdg_rx_*` receive path): classify v4 packets; `handle_v4_session_info` owns **session**, **announced** codec/rate/preroll, and when to **tear down** audio.
2. **Stale prior-session filter** (`dashcdg_rx_is_stale_prior_session_media_locked`): compares `header.sender_time_ms + preroll` to `session_start_ms`. TX may set `session_start_ms` to a **future** wall anchor (`now + warmup`); legitimate first packets were dropped until `v4_session_epoch_anchor_sender_ms` was introduced to avoid misclassifying fresh media (see block comment at `DASHCDG_RX_V4_SESSION_REORDER_SENDER_SLACK_MS` in `app_rx.c`).
3. **Audio jitter** (`dashcdg_audio_jitter_*`): ordered by `media_sequence`; drain consults starvation gate (see P3 RCA).
4. **Decode** → `dashcdg_desktop_audio_queue_frames` → **PCM ring** in `desktop_audio.c`.
5. **PortAudio / WinMM** callback: pulls ring; `timestamp_ms` used for HUD / sync; underruns increment counters used by recovery.

### 1.2 Speaker path → observability (“out” for diagnostics)

HUD / stats read **ring depth**, **DAC-ish** `timestamp_ms`, stall timers, and repair counters — not the reverse of playback.

---

## 2. Session bootstrap and `claim_audio_start`

**`dashcdg_rx_claim_audio_start_locked`** returns 1 only when:

- Network audio enabled, decode not disabled, stream not already started / not in flight, `g_audio` non-null, **`have_clock`**.
- Ring buffered ms ≥ target from `dashcdg_rx_audio_target_buffer_ms_locked`.
- **`dashcdg_rx_sender_playback_now_locked`** succeeds — which requires **`playback_base_sender_ms != 0`** (clock_sync or bootstrap from first audio chunk in `dashcdg_rx_store_v4_audio_frame_locked`).

**Wedging:** If bases stay 0, or sender timeline disagrees with queued `playback_ms`, preroll never satisfies “sender playback now” checks and audio never **starts** even with packets.

**`handle_v4_session_info`** must:

- On a **real** track/session change: stop stream, flush ring, `receiver_state_reset`, clear `g_audio_stream_started`, then `dashcdg_rx_configure_audio_locked` when params require it.
- Avoid **reconfiguring PortAudio every 1 Hz** on identical periodic `session_info` (see comments around `need_audio_device_reconfigure`).

---

## 3. Root causes audited (2026-04)

### 3.1 Cold join: `session_start_ms` was 0, so `session_changed` was false

`session_changed = (state->session_start_ms != 0 && ...)`. On **first** `v4_session_info` after process start, `state->session_start_ms` is **0**, so **no** session change is detected and **`material_track_change`** can be false when `song_id` is still empty (so `song_id_track_changed` is false) and asset metadata does not change. Then **`receiver_state_reset` is skipped** on first adopt of a live session.

**Symptom:** Odd first-join behavior (stale counters, jitter not in a clean session shape) until another packet path fixes state; interacts badly with **claim_audio_start** / preroll on slow hosts.

**Remediation:** Treat **adopting a non-zero session from a zero baseline** as a **material** track change (cold session adopt), so `receiver_state_reset` + stream teardown match the intent of “new epoch from silence.”

### 3.2 Same-ms TX session collision (documented in code)

If TX emits the **same** `session_start_ms` for two loads with the **same** `song_id` and **same** `asset_size`, `material_track_change` may stay false; jitter/PCM can splice. **Primary fix is TX** (monotonic `session_start_ms`). RX cannot infer a new track from wire fields alone in that case; **cold join** (§3.1) and normal `session_changed` cover every path where `session_start_ms` **differs** from the receiver’s previous value.

### 3.3 P3 / GDI / PortAudio stalls

Covered in `desktop-rx-p3-gdi-audio-stall-rca.md` (starvation gate, `buffered_silent` +1 ms timestamp noise, `Pa_IsStreamActive` vs `playback_running`).

### 3.4 Upstream TX seek / first frames

If the sender **skips** the start of the MP3, RX will only play what arrives on the wire; **no** RX-only fix replaces a bad sender seek. The TX `audio_producer_seek_to_zero` fix addresses that class.

---

## 4. Remediations implemented in code (this tranche)

1. **Cold session adopt:** `material_track_change` includes `state->session_start_ms == 0U && view->v4_session_info.session_start_ms != 0U` so the first adopt of a non-zero session from a zero baseline always runs the same path as a real track change (`receiver_state_reset`, stream teardown, reconfig as needed). **CDG-only** (no network audio) is included; the condition does not use `has_network_audio`.
2. **`has_network_audio` order:** Computed once, **before** `material_track_change`, so the flow is clear and the block is not split by a second late assignment.
3. **`need_audio_device_reconfigure`:** Also OR `view->v4_session_info.session_start_ms != prev_session_start_ms` (captured at handler entry). For stable 1 Hz `session_info` this is false; it matches **`new_v4_session_epoch`** defensively so a **new wire epoch** always reopens the PCM path when codec params are unchanged (and covers odd orderings where `material_track_change` heuristics alone might miss a required reopen). It does **not** fix the **identical** `session_start_ms` re-use case in §3.2 (TX must stay monotonic).

---

## 5. Verification

- Soak: first join to live TX, `n`/`b` on TX, and end-of-track auto-advance; watch RX HUD for `wait-preroll` / `audio buf` and listen for clean track starts.  
- Regressions: periodic `session_info` at 1 Hz must **not** stop/reopen the device (no change in `session_start_ms` and no spurious material).  
- P3: existing stall / recovery tests and the 500 ms jitter test in `test_core.c` remain valid.
