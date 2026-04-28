# RCA: Silent audio on connect / late join; `D`×2 recovery ≈ 3 s (desktop RX)

**Flight risk:** No launch audio on v4 multicast; user recovery by toggling decode-drop (`d` twice) restores output briefly.

**Observables:**

- **Silent** after connect or **late join** until manual intervention.
- **Press `d` twice** → `dashcdg_rx_toggle_audio_decode_drop` → decode disabled then enabled → **`dashcdg_rx_configure_audio_locked`** runs on re-enable.
- **~3 s** of audio after that matches **`DASHCDG_RX_QUEUE_SERVO_WARMUP_MS` (3000)** set on every `configure_audio` (servo held mild during warmup, not the sole bug).

---

## Fault tree (condensed)

| Symptom | Mechanism | Evidence in code |
|--------|-----------|------------------|
| **No sound, HUD “wait-preroll” / no `g_audio_stream_started`** | **`dashcdg_rx_claim_audio_start_locked`** requires **`have_clock`**, ring **≥ target**, and **`dashcdg_rx_sender_playback_now_locked`**. Sender timeline needs **`playback_base_sender_ms != 0`**. | `app_rx.c` `claim_audio_start` + `sender_playback_now_locked` |
| **Bases never set** | First v4 audio used to set **`playback_base_*` only if `have_clock`** (from `session_info` / announce / `clock_sync`). **Audio-before-control** reorder left **`have_clock` false** and **bases 0** → **`sender_playback_now` always false** → jitter **stall** / preroll never satisfies. | `dashcdg_rx_store_v4_audio_frame_locked` (fixed) |
| **Prefetched audio wiped** | First **`v4_session_info`** triggered **cold adopt** → **`receiver_state_reset`** cleared **jitter + bases** *after* audio had already been received, and **`configure_audio`** also **`dashcdg_audio_jitter_clear`**. | `handle_v4_session_info`, `receiver_state_reset`, `configure_audio` |
| **Packets never ingested (dominant)** | **`handle_v4_audio_chunk`** used to return when **`!network_audio_enabled`**, and **`network_audio_enabled`** is only set from **`v4_session_info`**. **Audio-before-session_info** ⇒ **all audio dropped** at the door — no `store_v4_*`, so no amount of store/bootstrap anchor logic helped until **`d` toggled** (which re-ran configure after session_info had arrived). | `handle_v4_audio_chunk` (fixed: `dashcdg_rx_bootstrap_network_audio_from_v4_chunk_locked`) |
| **`d` fixes it** | **`dashcdg_rx_set_audio_decode_disabled_locked(0)`** calls **`configure_audio`** with fresh path; **re-primes** decode/ring (same idea as stall recovery). | `dashcdg_rx_set_audio_decode_disabled_locked` |

---

## Remediations (this change set)

1. **`store_v4_audio`:** On first chunk, always set **`playback_base_ms` / `playback_base_sender_ms`** when zero; if **`!have_clock`**, **`dashcdg_media_clock_anchor`** from local wall + chunk **`sender_time_ms`** and set **`have_clock`**. Unblocks **claim** + **jitter drain** under **audio-first** reorder.
2. **`handle_v4_session_info`:** If **cold adopt** but **jitter already has frames** and **playback_base** was bootstrapped, **do not** run **`receiver_state_reset`** (only stop/flush host output, clear **audio FEC** scratch, keep **encoded** jitter + clock/bases).
3. **`configure_audio`:** Do not **clear reorder buffer / free decoders** when params **match applied** or when **first** apply with **`!rx_audio_applied_valid` but jitter already occupied** (encode path already running from live wire). Do not zero **playback bases** in the “cold reopen” stanza when we are **preserving prefetched** state. Still (re)open PCM ring and set **servo warmup** as today.
4. **`need_audio`:** Gate session-line extra disjunction with **`prev_session_start_ms != 0U`** so **0→T** cold join does not add a second, redundant configure signal (material already true when needed).

---

## Verification (T-30 style)

1. **Reorder path:** TX running; RX start with **session_info** after several **audio** datagrams (simulate with capture or flaky network). Expect **sound** without `d`.
2. **Happy path unchanged:** **session_info** before **audio** → full **reset** when jitter empty → no regression.
3. **1 Hz session_info:** **No** extra **configure** churn; `prev`/`view` same, `material` false.
4. **Double-`d`:** Still works; should be **unnecessary** for normal join.

---

## Not in this tranche

- **Identical `session_start_ms`** for two TX loads (TX monotonic session id).
- **P3 / WinMM long stall** (see `desktop-rx-p3-gdi-audio-stall-rca.md`).
