# ESP32 Badge Build/Flash/Debug Runbook (Windows)

This is the known-good workflow for `platform/espidf/projects/dashcdg_badge` on Windows.

## Preconditions

- ESP-IDF is installed under `C:\Espressif`.
- Badge is connected on the expected serial port (currently `COM6`).
- Use native `powershell.exe` or `cmd.exe`.
- Do not run ESP-IDF activation from Git Bash/MSYS shells.

## One-command build + flash (PowerShell)

Use the committed script:

`scripts/idf_build_flash_badge.ps1`

It does:

1. `Initialize-Idf.ps1` for `esp-idf-20ee62e792ea89630ac6a777ab3ebc57`
2. `cd` into `platform/espidf/projects/dashcdg_badge`
3. `idf.py --version`
4. `idf.py -p COM6 build flash`

Run it from native PowerShell:

```powershell
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\scripts\idf_build_flash_badge.ps1"
```

## Build only (CMD helper)

Use:

`scripts/idf_build_badge.cmd`

This is useful for quick compile checks without flashing.

## Post-flash debug cycle

Optional full automation switch:

- Enable `CONFIG_DASHCDG_BADGE_DEBUG_AUTO_LAUNCH_KARAOKE_AFTER_DHCP=y`
- Behavior: after first STA DHCP lease on each boot, badge opens Karaoke once; audio/video decode follow **Settings / NVS** (not overridden by this flag).

1. Install serial dependency (one-time):

```powershell
python -m pip install --user pyserial
```

2. Run one capture cycle:

```powershell
python "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\scripts\esp32_badge_debug_cycle.py" --skip-build-flash --iterations 1 --log-seconds 120 --port COM6
```

3. Wait for DHCP marker in logs, then launch CDG app on badge for playout validation.

4. Summarize capture:

```powershell
python "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\scripts\esp32_badge_log_summary.py" "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\docs\ops\logs\esp32-debug-cycle\<log-file>.log"
```

## Known failure mode and fix

- If activation fails with `MSys/Mingw is not supported`, the process inherited MSYS/Git-Bash environment variables.
- Fix: launch native `powershell.exe`/`cmd.exe` directly and run the scripts there.

## Build + flash + UART log (Path A script)

From repo root (adjust `COM6` if needed):

```powershell
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "scripts\idf_flash_then_capture_badge.ps1" -Port COM6 -CaptureSeconds 90
```

Output defaults to `scripts\badge_uart_last.txt`.

## Karaoke HUD “loss” pegged at 100% after track/session change (fix + verification)

**Symptom:** Top status line loss percentage (e.g. `100.0%`) stayed maxed after `V4_SESSION_INFO` / new song, even when audio sounded fine.

**Cause:** `wire_missing_estimate` / `header.sequence` gap tracking was **not** reset on new session while the sender can reset or jump sequence. Cumulative `wire_missing_estimate` vs `datagrams` blew up, so `karaoke_ui.c`’s 2s delta window reported ~100% “loss”.

**Fix (firmware):** `badge_rx_reset_for_new_session_locked` in `badge_rx.c` now clears `s_wire_seq_inited` / `s_wire_next_expected` / `s_wire_seen_bitmap` and zeros `s_stats.datagrams`, `s_stats.wire_missing_estimate`, and `s_stats.wire_reorder_events` so HUD and `v4_rx_stats` `loss_pct_x100` match the **current** session.

**UART proof:** After each new session you should see:

```text
badge_rx: session: baselined wire seq + loss counters (HUD loss % / v4_rx_stats track this session; dg=0 wm=0 reo=0)
```

Then `audio_chop` lines should show `d_wm` in proportion to real `d_rx` / `d_udp` again (not a permanent 100% HUD lie from stale wire state).

**On-device proof:** Advance a track or reconnect TX; the loss field on the karaoke bar should **drop** from a bogus peg and move with real link quality instead of staying at `100.0%`.

## UART: which numbers match “what you hear”

From `badge_rx_maybe_uart_log_audio_stats` (`audio_only` + `audio_chop` lines in `scripts/badge_uart_last.txt`):

| Field | Meaning | Aural correlate |
|------|---------|------------------|
| `jb=`**`occ/cap`** in `audio_chop` | Same ring depth as HUD **`a%`** (≈ `occ*100/cap`) | High sustained **occ≈cap** → full ring / latency; **0** with skips → starved |
| **`d_skip`** | Audio jitter **SKIP** steps in the log interval | **Primary breakup / PLC / “hash”** when it climbs |
| **`d_deg`** | Degraded / concealment pushes | Audible “loss fill” when non‑zero |
| **`d_wm`** | `wire_missing_estimate` delta | Wi‑Fi / socket **ingress loss** (not codec-only) |
| **`ms_since_pcm`** (`audio_only`) | ms since last successful apply to DAC path | **Gaps / dropouts** when large with low `jb` |
| **`d_ru` / `d_rs`** | Host underrun / silent stall deltas | **Underrun clicks or silence** |

## Karaoke HUD `aNN` pegged at 100 (real full audio jitter ring)

**What `a%` is:** `karaoke_ui.c` prints audio jitter **occupied slots × 100 / capacity** — not wire loss.

**When it pegs at `a100` for real:** the ring is actually **full** (ingress + inserts outrunning playout/drain), not a UART formatting glitch.

**Mitigation (firmware):** `badge_rx_deferred_audio_drain_if_pending_locked` scales the **post-`recvfrom`** deferred `badge_rx_drain_v4_audio_budget` step cap up (to `BADGE_RX_AUDIO_DRAIN_DEFERRED_MAX_STEPS_AUDIO_ONLY`, default 32) when `dashcdg_audio_jitter_occupied_count` is high, so bursty Wi‑Fi delivery does not leave the ring pegged at capacity between idle timer drains. Insert still only does `dashcdg_audio_jitter_insert` under the pump mutex; deep drains run after the socket read returned.

