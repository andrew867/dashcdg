# Root cause analysis: Windows 11 TX “no audio at track start,” playlist `n`/`b` / auto-advance, and build link errors

**Status:** Document first, then code. The first code change in the repo for the *audio* track (not the Makefile) is the `audio_producer_seek_to_zero` path in `app_tx.c` (see “Remediations (code)”).

## 1. Build failure: undefined `dashcdg_tx_ui_ncurses_*` (2025-04-27)

**Symptom:** `ld` reported undefined references to `dashcdg_tx_ui_ncurses_init`, `dashcdg_tx_ui_ncurses_present_lines`, `dashcdg_tx_ui_ncurses_drain_key`, `dashcdg_tx_ui_ncurses_shutdown` from `desktop_app_tx.o` and `desktop_app_tx_gdi.o` when linking `desktop-tx`, `desktop-gdi-tx.exe`, and `desktop-player`, but not all of those link lines included `tx_ui_ncurses.o`.

**Root cause:** `platform/desktop/src/app_tx.c` always calls the ncurses helper (the object is a **stub** when `DASHCDG_TX_UI_NCURSES` is not defined; the same **symbol names** are still required at link time). Only the `desktop-tx` rule was updated to pass `$(TX_UI_NCURSES_OBJ) $(TX_NCURSES_LDLIBS)`.

**Fix:** Add the same object and optional `$(TX_NCURSES_LDLIBS)` to every link that includes any `desktop_app_tx*.o` (player, GDI TX, retro TX, and the main `desktop-tx` rule that was already correct).

**This is not an audio defect;** it blocked full `make debug` on MinGW.

---

## 2. Symptom set (user report)

- **OS / hardware:** Windows 11, multiple machines (HP, Lenovo).
- **Regressions after unifying “next” with one code path:** `n` / `]` and `b` / `[` use `dashcdg_tx_advance_playlist_locked` (playlist-based). **End-of-track** auto-advance in the main TX thread also calls the same function. **Session “back/forward in history”** used to be a different path (`dashcdg_tx_load_history_delta_locked`); that function is still in the tree but is not the default for the `n`/`b` keys in the current `dashcdg_tx_handle_command` (it is `DASHCDG_TX_MAYBE_UNUSED` and not wired to the GLUT / GDI paths in the paste the user had).
- **PortAudio / “player”:** Receivers use `platform/desktop/src/desktop_audio.c` and the desktop **RX** path (WASAPI, stream open, underrun / recovery). The **TX** path does not play to the user’s speakers; it **encodes** and sends. “Stream fails quickly” and “no sound on the machine” for a *viewer* should be triaged on **RX** or **desktop-player** local preview, not the same `Pa_*` path as TX. This document still summarizes **TX → network** and **RX → PortAudio** in one place for orientation.

---

## 3. Data flow (end to end, two passes)

### 3.1 “Out to in” (TX: file / wall time → wire)

1. **Control or main thread (with `g_tx_state.mutex` held):** `dashcdg_tx_load_track_locked` or `dashcdg_tx_advance_playlist_locked` → `dashcdg_tx_load_track_with_history_locked` / `dashcdg_tx_load_relative_track_locked`.
2. **CDG:** load, optional trim, `cdg_source` and `reader` for preview/headless path; `dashcdg_tx_build_cdg_batches_locked`, v4 hazard, `dashcdg_tx_recompute_v4_anchor_interval_locked`.
3. **Audio (in-process “ready” queue):** `dashcdg_tx_build_audio_frames_locked` clears the audio ready queue, bumps `g_tx_state.audio_pipeline_generation` (this is the only place the **main** track build does that; **codec-only** changes also bump generation in `dashcdg_tx_cycle_v4_audio_codec_locked` / `dashcdg_tx_select_v4_audio_codec_locked` without calling `build_audio_frames_locked`).
4. **Session / v4:** `playback_anchor_local_ms` and `session_start_ms` are set; `dashcdg_tx_send_v4_track_bootstrap_locked` sends `session_info` and `clock_sync` so RX can rebase.
5. **Audio producer thread** (`dashcdg_tx_audio_thread_main` in `app_tx.c`): on `audio_pipeline_generation` change, closes the old minMP3 (or similar) source, may reinit narrowband encoders, `strdup`’s the new `mp3_path`, and **opens** the new file. After a successful open it has always run:
   - `resume_ms = dashcdg_tx_current_playback_ms_locked(dashcdg_clock_now_ms())` (with mutex),
   - then `dashcdg_desktop_audio_seek_mp3_stream(source, (uint32_t) resume_ms)`.

6. **Network send path** (v4): `tx_audio_send` and `tx` thread pack and send; `dashcdg_tx_ad_sync_from_main_locked` copies `session_start_ms` and `audio_pipeline_generation` into the `g_tx_ad` side for the send path.

**Conclusion of the out-to-in pass:** The only place that can make a **new** track’s first audio “start in the wrong place” in the **file** is the **MP3 seek** in the producer, which is supposed to match the **session** `playback_ms` that the main state is using.

### 3.2 “In to out” (RX: wire → decode → PortAudio)

1. **UDP** receive, v4 parse, Opus (or other) decode, jitter / playout.
2. **Desktop audio** (`desktop_audio.c`): `Pa_OpenStream` (WASAPI preferred on Windows), callback pulls from a ring and can return partial / silence.
3. **Stall / recovery** in `app_rx.c` (see `docs/specs/desktop-rx-p3-gdi-audio-stall-rca.md` and in-file comments around `claim_audio_start`, `Pa_IsStreamActive`, post-`q` dead backend, etc.).

**Conclusion of the in-to-out pass:** “Stream opened then failed” on the **listener** is a different failure class (device, host API, backpressure, or **stale session** on the wire if TX did not reset the session). v4 `track_bootstrap` in `load_track` is there so RX is not stuck on the *old* `session_start_ms` while new media is already on the wire.

---

## 4. Root cause: wrong **MP3 seek** on **new track** (main finding for “no / bad audio at start of every new track”)

**Location:** `app_tx.c` in the audio producer, after `dashcdg_tx_audio_open_source` succeeds; the code that does:
`resume_ms = dashcdg_tx_current_playback_ms_locked(dashcdg_clock_now_ms())` and then seeks the **new** file to `resume_ms`.

**Intended use of that block (from the in-file comment):** “codec hot-swap” and “align decoder to the current **session** timeline” so frame `playback_ms` tags and the v4 send path do not sit for minutes with a **mismatched** file position (e.g. after `c` to change codec on the **same** file). In that case, the file is the same, the **media** position is not 0, and **wall-derived** `current_playback_ms_locked` is the right **first** seek target (clamped to `duration_ms`).

**What goes wrong on a **new** track after `dashcdg_tx_build_audio_frames_locked`:**

- The new track sets `playback_anchor_ms = 0` and `playback_anchor_local_ms` to “now + optional warmup” in `load_track_locked`.
- `dashcdg_tx_current_playback_ms_locked` returns:
  - `0` while `now_ms <= playback_anchor_local_ms` (pre-warmup);
  - `0 + (now_ms - playback_anchor_local_ms)` **after** the wall time has passed the anchor.
- The **audio producer** can run **tens to hundreds of ms** after the main thread finished `load_track` (thread scheduling, minMP3 open, encoder init on Windows 11). If that moment is **after** the anchor, the function returns a **non-zero** `resume_ms` for a track that is still “at the start of the file” in **content** terms. The producer then **seeks the new MP3** to that many **milliseconds** from the start. That:
  - **skips** the beginning of the track (perceptual: “no intro / late start”);
  - on a **short** track, can **seek to the end** (or past, then **capped** to `duration_ms`), with the next read path looking like “no audio for a long time” or **EOF** on the first read, i.e. “stream fails right after open” from the perspective of the send path.

**Why it can look like “playlist vs history”**

- `dashcdg_tx_load_history_delta_locked` calls `dashcdg_tx_load_track_locked` only; `dashcdg_tx_advance_playlist_locked` → `load_track_with_history_locked` → the same `load_track_locked` and the same `build_audio_frames_locked` and the same seek. The **file** path is the same; the difference is **which** index is chosen and **wrap+shuffle** in `load_relative_track_locked` (not in the short history path that only steps within `track_history`). The “history path works, playlist does not” report is **not** explained by a different `load_track` for the file; it is consistent with:
  - **more** playlist / auto-advance events (so the bad seek runs more often);
  - or different user flow (e.g. end-of-track while at the end of the list **triggers wrap+shuffle**, or more time has passed so the producer is more often “late” past the anchor).
- A full second pass on “wrap + shuffle” and `next_index` is a follow-up if any case still mis-routes after the seek fix.

---

## 5. Remediations

### 5.1 Makefile (link all `app_tx` consumers with `tx_ui_ncurses.o`)

*(Done in the same change set as this file; see `Makefile` for `PLAYER_BIN`, `TX_GDI_BIN`, `RETRO_TX_BIN`.)*

### 5.2 Code: new track / new `build_audio_frames` must **not** use wall-based first seek

- Add a flag, e.g. `g_tx_state.audio_producer_seek_to_zero`, set to 1 only in `dashcdg_tx_build_audio_frames_locked` (the function that runs for every `load_track` that loads a new media build, and **not** for codec-only `audio_pipeline_generation` bumps that do not go through it).
- In the audio producer, when that flag is set, use `resume_ms = 0` (and clear the flag under the same mutex as the read of `current_playback_ms`), so the new file always decodes from the start. **Codec hot-swap** (no `build_audio_frames` in that code path) leaves the flag clear and keeps the wall-based seek.

### 5.3 PortAudio / RX (if problems remain on the **listener**)

- Re-run with the fixed TX and collect `desktop-rx` or `desktop-gdi-rx` logs; use the existing stall/diagnosic text in `app_rx.c` and `desktop_audio.c`.
- This document does not change the PortAudio open order; it only ensures the **TX** side does not **skip** the first half second of every new file on a slow producer.

---

## 6. References in tree

- `platform/desktop/src/app_tx.c` — `dashcdg_tx_load_track_locked`, `dashcdg_tx_build_audio_frames_locked`, `dashcdg_tx_load_relative_track_locked`, `dashcdg_tx_advance_playlist_locked`, `dashcdg_tx_load_history_delta_locked`, audio producer open+seek.
- `platform/desktop/src/desktop_audio.c` — PortAudio open, WASAPI default, errors.
- `platform/desktop/src/app_rx.c` — receive, callback, recovery, comments on `Pa_IsStreamActive`.
- `docs/specs/desktop-rx-p3-gdi-audio-stall-rca.md` — related RX-side stall work.
