# End-to-end RCA: TX/RX desktop path (MP3 + CDG → UDP → PCM → PortAudio)

**Scope:** Desktop **TX** (`platform/desktop/src/app_tx.c`) and **RX** (`platform/desktop/src/app_rx.c`), with **PCM I/O** in `platform/desktop/src/desktop_audio.c`, **audio reorder buffer** in `core/src/audio_jitter.c`, and **CDG batch** reorder in the corresponding CDG jitter module. This document traces **both directions** along the real data path and lists **joint** failure modes.

**Related RCAs (deeper dives):**

- `docs/specs/desktop-tx-audio-pipeline-playlist-advance-rca.md` — TX seek, session bootstrap, encoder alignment.
- `docs/specs/desktop-rx-audio-pipeline-rca.md` — RX session_info, cold join, `claim_audio_start`.
- `docs/specs/desktop-rx-p3-gdi-audio-stall-rca.md` — RX long-run stalls, starvation gate, WinMM/PortAudio behavior.

---

## 1. Shared timeline model (what must stay consistent)

Both sides anchor **media time** to a single integer line on the wire:

- **`session_start_ms`** — TX wall-clock **epoch** for the current track load (after optional warmup). Every audio chunk and CDG batch carries a **`playback_ms`** offset **from track start**; **absolute** media time on the sender is conceptually **`session_start_ms + playback_ms`**.
- **`header.sender_time_ms`** — wall time when the **datagram** was sent (used for clock sync, stale-session heuristics, and RX rate observations — **not** a substitute for `playback_ms` in the media payload).

If **MP3 decode position**, **encoded `playback_ms`**, **send scheduling**, and **RX clock mapping** disagree, you get classic symptoms: HUD stuck on **wait-preroll**, audio ahead/behind graphics, or CDG **render_gate** stuck while audio plays.

---

## 2. Forward path: files → speaker (TX then RX)

### Stage A — Files and track load (TX)

| Step | What happens | Primary code |
|------|----------------|--------------|
| A1 | Playlist entry resolves to **CDG** (and optionally **MP3**) paths. | `dashcdg_tx_load_track_locked` |
| A2 | CDG file is mapped/read into a **`cdg_source`**; **duration** is derived from subchannel packet count (and may be extended by MP3 duration). | Same; `dashcdg_packet_count_to_ms`, `dashcdg_tx_get_audio_duration_ms` |
| A3 | **CDG batches** are precomputed: the file is sliced into wire batches, each with **`playback_ms`** from packet index (`dashcdg_packet_count_to_ms(start_packet)`), **`media_sequence`**, and FEC **group_id / group_index**. | `dashcdg_tx_build_cdg_batches_locked` |
| A4 | **Hazard scan** (XOR density, keyframe gaps) chooses bootstrap/anchor tuning for difficult CDGs. | `dashcdg_tx_assess_cdg_hazard_locked` |
| A5 | **Audio pipeline generation** for this track: clears encoder queues, bumps **`audio_pipeline_generation`**, sets **`audio_producer_seek_to_zero`** so the producer reopens the MP3 from a defined position aligned with the new timeline. | `dashcdg_tx_build_audio_frames_locked` |
| A6 | **Session epoch:** `playback_anchor_local_ms = now_ms + warmup` (optional), then **`session_start_ms = playback_anchor_local_ms`**. TX forces **strict monotonicity** vs the previous wire session id so two loads in the same wall millisecond still advance **`session_start_ms`** (RX sees a real session change). | `dashcdg_tx_load_track_locked` (block ~5154–5168) |
| A6b | **`g_tx_ad` sync:** `dashcdg_tx_ad_sync_from_main_locked()` runs in the same critical section right after the new `session_start_ms` is published, so the **audio send thread** does not release frames for up to a main-tick using a **stale** `g_tx_ad.session_start_ms`. | Same, before `dashcdg_tx_send_v4_track_bootstrap_locked` |
| A7 | Legacy **announce/beacon** and **v4 bootstrap** notifications go out as needed. | `dashcdg_tx_send_v4_track_bootstrap_locked`, announce fields |

**RCA note:** If A6 were not monotonic, RX could **skip** `receiver_state_reset` when `song_id` and `asset_size` collide — jitter and PCM **splice** across tracks (addressed on TX; RX still has cold-join and asset-size edges).

---

### Stage B — MP3 → PCM → codec → send queue (TX producer)

| Step | What happens | Primary code |
|------|----------------|--------------|
| B1 | **Producer thread** opens the MP3 via **`dashcdg_desktop_audio_open_mp3_stream`**, optionally initializes **Opus** (or other v4 codec). | `dashcdg_tx_audio_open_source`, `dashcdg_tx_init_audio_encoder_for_codec` |
| B2 | Reads PCM with **`dashcdg_desktop_audio_read_mp3_frames`**, resamples to the wire rate/channel layout, encodes fixed **`frame_ms`** frames. | Audio thread loop in `app_tx.c` (~6590+) |
| B3 | After hot codec swap or track logic, **MP3 stream is seeked** to **`resume_ms`** aligned to **`dashcdg_tx_current_playback_ms_locked`**, optionally **forced to 0** when **`audio_producer_seek_to_zero`** is set — keeps **`playback_ms` tags** on the wire aligned with the **session timeline** (avoids “audio at 0.. while session wall clock says minute 5”). | Comments ~6524–6543; seek path |
| B4 | Encoded frames are pushed to **`audio_ready_queue`** with **`media_sequence`**, **`playback_ms`**, FEC group fields. | `dashcdg_runtime_queue_*`, `dashcdg_tx_build_audio_frames_locked` reset + producer |

**RCA note:** Slow MP3 decode (disk/CPU) shows up as **queue starvation**, **send gaps**, and **silence fill** in the send path; bursty sends show as **jitter** on RX.

---

### Stage C — Release to the network (TX send path)

| Step | What happens | Primary code |
|------|----------------|--------------|
| C1 | **Main TX tick** (`dashcdg_tx_tick_v4_locked`) periodically sends **`v4_session_info`** (~1 Hz) and **clock_sync**, and moves **CDG video deltas / anchors** along **`dashcdg_tx_media_send_deadline_ms_locked`** — explicitly **not** tied to the encoder’s internal “network playback” clock for gating, so slow encode does not permanently stall **release** of already-tagged frames. | `app_tx.c` ~7904+ |
| C2 | **Audio send** uses **`send_deadline_ms ≈ now + lead`**; a frame ships when **`session_start_ms + playback_ms ≤ send_deadline`**. If the queue is late, TX may **wake-late** count, or **silence-fill** to preserve timeline continuity. | `dashcdg_tx_send_due_audio_locked` (~4558+) |
| C3 | **v4 audio chunk** serialized with **`playback_ms`**, codec id, **`media_sequence`**, FEC **group_id/index**. **`header.sender_time_ms`** set to **now** at send. | `dashcdg_tx_send_v4_audio_chunk_locked` |
| C4 | **CDG batches** follow the same **deadline** discipline with **`playback_ms`** per batch. | `dashcdg_tx_tick_v4_locked` + CDG send helpers |

**RCA note:** Splitting **encode** (producer) from **send** (deadline + lead) is why fixing “seek to zero after batch build” matters: the **tags** must match the **epoch** chosen in stage A6.

---

### Stage D — Wire (logical)

Datagrams carry:

- **Control:** `v4_session_info` (codec, rate, preroll, **`session_start_ms`**, asset metadata), `v4_clock_sync`, loading/anchor modes.
- **Media:** `v4_audio_chunk`, `v4_video_delta` (CDG batches), repair/FEC packets.

**Trust boundary:** Anything not authenticated here is **untrusted**; receivers must validate sizes, sequence, and session epoch before touching decode buffers.

---

### Stage E — RX ingest and session binding

| Step | What happens | Primary code |
|------|----------------|--------------|
| E1 | Receive loop classifies packets and dispatches **handlers** (`handle_v4_session_info`, `handle_v4_audio_chunk`, CDG handlers, etc.). | `app_rx.c` receive path |
| E1b | **Legacy `announce`** (v3) uses the same **cold session adopt** idea as v4: if `session_start_ms` was 0 and the announce carries a non-zero session, **`receiver_state_reset`** and stream re-init run so a v3-only path is not stuck in a pre-session jitter shape. | `handle_announce` |
| E2 | **`v4_session_info`** updates announced codec/rate/preroll, **`session_start_ms`**, song id, asset size; on **material** change (session/song/asset/cold join) stops audio, flushes ring, **`receiver_state_reset`**, then may **`dashcdg_rx_configure_audio_locked`**. **`need_audio_device_reconfigure`** also keys off **`session_start_ms != prev`** vs last handler entry so a new epoch reopens PCM when codec params are unchanged. | `handle_v4_session_info` |
| E3 | **`v4_clock_sync`** (and related beacons) drive **`media_clock`**: maps between **local wall** and **sender** time; **`have_clock`** becomes true when healthy. | Handlers + `dashcdg_media_clock_*` |
| E4 | **Stale prior-session filter** drops audio whose **sender time** looks like it belongs to an **old** `session_start_ms` (with slack for TX warmup / reorder — see `DASHCDG_RX_V4_SESSION_REORDER_SENDER_SLACK_MS` and `v4_session_epoch_anchor_sender_ms` on fresh epoch). | `dashcdg_rx_is_stale_prior_session_media_locked`, `handle_v4_audio_chunk` |

**RCA note:** Without E2 cold-join treatment, the **first** non-zero session could skip **reset** while counters/jitter still look like “no session.” Without E4, **warmup** can misclassify **legitimate** first packets as stale.

---

### Stage F — Reorder, decode, PCM ring, host

| Step | What happens | Primary code |
|------|----------------|--------------|
| F1 | Audio chunks **`dashcdg_rx_insert_audio_pending_locked`** → **audio jitter** keyed by **`media_sequence`**, with **`playback_ms`** per slot. | `dashcdg_rx_store_v4_audio_frame_locked` → jitter |
| F2 | **Drain** (`dashcdg_audio_jitter_drain_step`) uses **starvation / skip** policy (see core) plus RX **sender playback now** vs **preroll** to decide when to emit the next frame; missing sequences become **skips** (hard resync logging on large gaps). | `dashcdg_rx_jitter_tick_locked` region ~4840+ |
| F3 | **`dashcdg_rx_apply_audio_frame_locked`** decodes (Opus, AMR, QCELP, SBC, …) to **48 kHz stereo** (or as implemented), optional **resample trim** servo when the **PCM ring** is near target fill. | `dashcdg_rx_apply_audio_frame_locked` |
| F4 | Decoded PCM → **`dashcdg_desktop_audio_queue_frames`** into the **stream ring** with a **timestamp** for HUD/sync. | `dashcdg_rx_queue_decoded_interleaved_pcm_locked` |
| F5 | **`dashcdg_rx_claim_audio_start_locked`** allows **`dashcdg_desktop_audio_start_stream`** only when: network audio on, decode enabled, **clock ready**, ring buffered **≥ target**, and **`dashcdg_rx_sender_playback_now_locked`** agrees — avoids starting DAC on an inconsistent sender timeline. | `dashcdg_rx_claim_audio_start_locked` |
| F6 | **PortAudio** (or **WinMM** on some builds) **callback** pulls from the ring (`dashcdg_pa_callback` / WinMM equivalent). Host latency is tuned for multicast parity (`DASHCDG_PA_NETWORK_STREAM_HOST_LATENCY_SECONDS` comment in `desktop_audio.c`). | `desktop_audio.c` |

**RCA note:** F5 is the usual **wait-preroll wedge** if **playback bases** are zero or **sender_playback_now** never lines up with queued **`playback_ms`**. F3 backpressure (ring full) **returns 0** but **CDG drain** is intentionally still run in the same tick so graphics do not freeze when audio blocks.

---

### Stage G — CDG (parallel path on RX)

| Step | What happens | Primary code |
|------|----------------|--------------|
| G1 | CDG batches land in **CDG batch jitter** (same broad pattern as audio: sequence, `playback_ms`). | `handle_live_cdg_batch`, `dashcdg_rx_store_cdg_batch_locked` |
| G2 | Drain tracks audio policy but **must progress** even under **PCM backpressure** so the UI does not stick on **black/connecting** while the ring is full. | Comment ~4960+ in `app_rx.c` |
| G3 | Render snapshot / HUD use **CDG state** + **DAC-ish** `timestamp_ms` from the audio path for **lip-sync** style presentation. | GL/GDI render paths, HUD builders |

---

## 3. Reverse path: PortAudio → mental model of MP3/CDG files

Read **upstream** from the DAC callback — what each layer **implies** about the original files and session:

| Layer | Backward interpretation |
|-------|-------------------------|
| **Host callback** | Consumes **already-mixed** PCM at device rate; **underruns** mean the ring was empty — causes are F4 not keeping up, F2 not draining, or E network loss. |
| **PCM ring / `timestamp_ms`** | Should track **sender `playback_ms`** + mapping through clock sync; if **jitter** or **clock** is wrong, timestamps disagree with **CDG `playback_ms`**. |
| **Decoder output** | Implies a **codec payload** on the wire matching **announced** `codec_id` / profile; garbage implies loss, bit-rot, or **wrong session** accept. |
| **Audio jitter slots** | Implies a **sequence** of **`v4_audio_chunk`** datagrams; holes imply **loss** or **reordered** delivery past repair; **next_media_sequence** stuck implies decode failure or backpressure never releasing a slot. |
| **Stale filter** | If **legitimate** early packets are dropped, you never prime **playback_base_*** from audio — **claim_audio_start** waits forever **even with data visible in captures**. |
| **`v4_session_info`** | Defines **which MP3/CDG “world”** you think you are in: **`session_start_ms`**, **asset_size**, **song_id**. Wrong epoch ⇒ interpreting **MP3 file position** vs **`playback_ms`** incorrectly. |
| **TX send deadline + producer** | Backward: **MP3 read position** must match **`playback_ms`** tags; **seek_to_zero** after batch build exists precisely so **file offset** and **wall epoch** line up after seeks and codec reloads. |
| **CDG batches** | Backward: **`playback_ms`** on a batch maps to **packet index** in the **CDG file**; **graphics** at time *t* should map to the **same** *t* on the MP3 timeline if both were authored together. |

This reverse reading is the checklist for **“it sounds right but graphics wrong”** vs **“nothing starts”**: split **clock/base** issues from **graphics-only** reorder issues.

---

## 4. Joint RCA table (symptom → likely locus)

| Symptom | Often caused by (TX) | Often caused by (RX) | Notes |
|---------|----------------------|----------------------|--------|
| HUD **wait-preroll** forever | `playback_ms` not aligned to **`session_start_ms`** after load/seek; send stalled | **`have_clock` false**; **playback bases** 0; **stale filter** dropping first chunks; **claim** predicates |
| Short **audio glitch** at track boundary | Same-ms **session** before monotonic fix; producer lag | **receiver_state_reset** skipped (cold join / same metadata) |
| **CDG black** while audio plays | N/A (graphics path) | **CDG drain** skipped historically when PCM ring full — code forces CDG step even on audio apply **0** |
| **Stall** minutes in (PIII/GDI) | — | **Starvation gate**, `Pa_IsStreamActive`, silent-stall repair — see P3 RCA |
| **Two receivers** out of sync in one room | — | Different **host output latency** negotiation; RX targets **ring** buffer, not only PA default |
| Bursty **TX send gaps** | Disk/slow MP3, CPU encode, **sendto** blocking | Playout **jitter** increases; may trigger **skips** |

---

## 5. What this document does *not* cover

- **ESP32 / badge** paths (different memory and transport constraints).
- **Security** (authentication, encryption) — treat the LAN segment as a separate policy decision.
- **Exact** packet binary layouts — see `proto` serializers and the v4 spec docs.

---

## 6. Suggested verification order

1. **TX alone:** load track, confirm **`session_start_ms`** advances every load, **`playback_ms`** starts at 0 after seek, and producer queue stays ahead of send deadline under stress.
2. **RX alone (recorded pcap or loopback):** cold join, then skip tracks; **`material_track_change`** and **`new_v4_session_epoch`** should align with session_info.
3. **End-to-end:** soak with **`n`/`b`**-style track changes and **auto-advance**; watch RX HUD **gate** lines and listen for clean boundaries.

This ordering separates **tag/timeline** bugs (fix near TX or clock path) from **host stall** bugs (fix near `desktop_audio.c` / P3 RCA).
