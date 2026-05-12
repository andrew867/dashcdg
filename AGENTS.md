# Agent / Codex handoff — dashcdg receiver (desktop-rx)

Last consolidated from session work (2026). Use this to resume debugging or implementation without re-deriving context.

## Agent execution contract (non-negotiable)

This section applies to **every** agent session on this repo. It exists so the operator is not asked to micromanage basics that the agent can resolve alone.

### ESP-IDF and `idf.py` — you do not get to “opt out”

You **must not** tell the operator that **`idf.py` cannot be run**, that **ESP-IDF is missing**, or that you **cannot build** the badge firmware **solely** because a one-off shell probe printed `command not found` or the IDE sandbox lacked `PATH`.

**Before** any such refusal, you **must** actually execute the **correct** documented activation for the host (see **`## Embedded / ESP32`**, **`## ESP32 badge: build / flash`**, **`docs/embedded/esp-idf-setup.md`**). **Do not** run `scripts/bootstrap_esp_idf.sh` on the operator’s primary Windows + Espressif machine when **`C:\Espressif\Initialize-Idf.ps1`** is the real toolchain — that wastes time and ignores explicit operator instructions.

### Cursor IDE on Windows — integrated terminal is usually Git Bash (non-negotiable)

When the agent runs shell commands from Cursor, the default shell is often **Git Bash / MSYS**. Child processes **inherit `MSYSTEM`, `MINGW_*`, etc.** ESP-IDF’s Python bootstrap then fails with **`ERROR: MSys/Mingw is not supported`** — this is **not** “IDF missing”; it is **wrong shell pollution**.

**Agents must not** paste bare `Initialize-Idf.ps1` / `idf.py` into Cursor bash and declare defeat.

**Use one of these every time (pick one):**

1. **PowerShell + repo scripts (recommended, Path A):** call **native** `powershell.exe` (full path below) so the **parent process is not Git Bash / MSYS**. The scripts clear MSYS-related env vars and invoke **`C:\Espressif\Initialize-Idf.ps1`** before `idf.py`. From repo root (adjust path if needed):
   - Build + flash (default port **COM6**; override with `-Port`):  
     `C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -File "scripts\idf_build_flash_badge.ps1"`  
   - Build only (no COM / no flash; for agents and CI): append **`-BuildOnly`** to the same `-File "scripts\idf_build_flash_badge.ps1"` invocation.
   - Build + flash, then UART capture (exclusive COM usage; same script family as automation):  
     `...\powershell.exe -ExecutionPolicy Bypass -NoProfile -File "scripts\idf_flash_then_capture_badge.ps1"`  
     Common flags: `-Port COM6 -CaptureSeconds 120 -SkipFlash` (UART only after a manual flash).
2. **Interactive PowerShell:** run the **`Initialize-Idf.ps1`** one-liner below, **`cd`** to **`platform\espidf\projects\dashcdg_badge`**, then **`idf.py build`**, **`idf.py flash`**, or **`idf.py build flash`** (or **`idf.py -p COM6 build flash`**).
3. **Path B:** stay entirely in **`bash`** and use **`source third_party/esp-idf/export.sh`** after bootstrap — **never** mix Path A `Initialize-Idf.ps1` with Git Bash on the same command line.

**Primary operator Windows machine — activate ESP-IDF in PowerShell (canonical; copy verbatim):**

```text
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoExit -File "C:\Espressif/Initialize-Idf.ps1" -IdfId esp-idf-20ee62e792ea89630ac6a777ab3ebc57
```

That is the **`Initialize-Idf.ps1`** install the operator uses. Open it, **then** in that same environment `cd` to `platform\espidf\projects\dashcdg_badge` and run **`idf.py build`**, **`idf.py flash`**, or **`idf.py build flash`** in one step (operator-verified in an ESP-IDF–loaded shell: Ninja runs under `…\dashcdg_badge\build`). Alternatively invoke **`scripts\idf_build_flash_badge.ps1`** or **`scripts\idf_flash_then_capture_badge.ps1`** via **`powershell.exe -ExecutionPolicy Bypass -NoProfile -File`** as in the list above. **`-NoExit`** keeps the window open for interactive work; for one-shot non-interactive automation you may drop `-NoExit` but keep the same `-IdfId`.

**Recovery order (pick the first that applies):**

1. **Windows + Espressif install (this operator):** run the **`Initialize-Idf.ps1`** line above (or Path A scripts below). **Do not** default to repo `third_party/esp-idf` bootstrap here.
2. **No Espressif install / Linux / CI / “repo-only” IDF:** if **`third_party/esp-idf/tools/idf.py`** is absent → **`bash scripts/bootstrap_esp_idf.sh`**, then **`source third_party/esp-idf/export.sh`** or **`bash scripts/build_esp32_freertos_platform.sh`**.
3. Only after the applicable path **fails when you actually ran it**, report **verbatim** errors—not “I can’t.”

**Path A** (native Windows + Espressif) vs **Path B** (Git Bash / WSL / Linux + repo `third_party/esp-idf`) are both valid; **on this operator’s Windows box, Path A + the command above wins** unless they say otherwise.

### “Not in the repo” — forbidden without evidence

You **must not** assert that a file, script, or directory **does not exist** in this repository without a **good-faith search** first (`Glob`, `Grep`, listing plausible parent dirs, checking `scripts/`, `docs/`, `platform/`). If search exhausts reasonable variants, say **what you searched** and ask for an exact path—do **not** state absence as fact from a single failed guess.

### Default stance: execute, then escalate with facts

Assume you **can** run shell commands, use the network (bootstrap, docs, web search for error strings), and iterate. “I can’t” is only acceptable after **documented attempts** hit a genuine hard block, and your message **must** include what you already tried and the **verbatim** failure—not a vague disclaimer.

## Current baseline (desktop protocol v4)

Windows **desktop-rx** / **desktop-tx** with **v4 multicast**: codec hot-swap (**Opus** + narrowband family **ids 2–7**), **cold join** (RX before TX), **idle RX then TX starts**, and **pause / unpause** are exercised in development with **stable A/V** after several remediation passes. User validation targets **win-x64** sneakernet builds (`windows-x64/desktop-rx.exe`, `desktop-tx.exe` from the same tree).

Treat **`platform/desktop/src/app_rx.c`**, **`platform/desktop/src/app_tx.c`**, **`platform/desktop/src/desktop_audio.c`**, **`platform/desktop/src/pcm_rate_convert.c`**, **`platform/desktop/src/opus_codec.c`**, **`core/src/audio_jitter.c`**, and **`core/src/cdg_batch_jitter.c`** as the primary runtime seams.

**TX (2026):** Audio send uses **`g_tx_ad`** (dedicated mutex + mirrored session
fields) and unified **wire** **`dashcdg_tx_next_wire_sequence()`**; **V4_RX_STATS**
ingests on the PTP thread and **applies in batch** on the main loop — see
**`docs/architecture/tx-audio-isolation.md`**. Periodic logs include
**`thread_deadline_miss`** and **`v4_rx_stats_drop`**.

**RX / HUD time:** `desktop_audio` updates **`timestamp_ms`** using
**`max`(`time_info` span, `Pa_GetStreamInfo` `outputLatency`)** on PortAudio so
on-screen CDG is not a few hundred ms **ahead** of heard audio on Windows/WASAPI.

v4 **full-file backfill** (cycling the `.cdg` on the wire) is **removed** from TX; `session_info.startup_backfill_mode` is **0**; RX does not assemble a local file from `V4_BACKFILL_CHUNK` and does not `calloc` the full CDG on v4 `session_info` join (`asset_size` remains metadata-only). The **first** decoded v4 anchor must still **`apply_snapshot_locked`** when `cdg_snapshots_applied == 0` so **`dashcdg_cdg_batch_jitter_apply_snapshot_seek`** runs before any CDG deltas — **`dashcdg_rx_should_apply_v4_anchor_locked`** must not require `cdg_batch_jitter.initialized` first (that deadlocked late join).

**v4 video anchor** chunks are **paced** (min interval between chunks; smaller per-datagram payload cap on TX) so periodic RLE anchors do not flood one fragment per TX tick.

## Root causes addressed in code (historical + retained behavior)

1. **Jitter empty-buffer runaway** — `primed_decode` gates empty / stall-loss skips until first successful decode (`core/src/audio_jitter.c`, `core/src/cdg_batch_jitter.c`). CDG jitter must not anchor **`next_packet_index`** to the first UDP datagram (reorder-safe insert at cursor 0 + stale drop + contiguous rewind in **`dashcdg_cdg_batch_jitter_drain_step`**).

   **Ahead-of-playback (lip-sync):** When **`have_sender_playback`** is set, **`dashcdg_cdg_batch_jitter_drain_step`** must not **APPLY** until **`dashcdg_packet_count_to_ms(batch_start) ≤ receiver_playback_now_ms + late_grace_ms`** (same delayed receiver timeline as late/catch-up). Otherwise contiguous batches in the ring could be applied in one host loop and **on-screen lyrics could run seconds ahead of the speakers** (fixed 2026).

2. **`handle_v4_session_info` teardown loop** — Reconfigure only when **audio fields** (or device null) change, not bare `asset_changed` while chunks are still settling (**`need_audio_device_reconfigure`** in **`app_rx.c`**).

3. **Live CDG before snapshot** — **`dashcdg_rx_seed_live_state_before_first_wire_delta_locked`** seeds **`live_state`** before first wire delta (**`app_rx.c`**).

4. **Narrowband DSP / switching** — Band-limited **48 kHz → 8 / 16 kHz** decimation, Lanczos / overlap SRC guards, startup skip-hold behavior for codec handoff vs cold join; **TX** ~80 Hz HPF + **~3 dB digital headroom** before narrowband encode; **Opus** encoder input uses the same Q15 gain for loudness parity; soft limiting on hot PCM where documented in **`pcm_rate_convert.c`**.

5. **Playback timeline (`playback_base_*`)** — Cleared on cold audio reopen (**`!rx_audio_applied_valid`**), when session_info disables network audio, and aligned with **`claim_audio_start`** so HUD does not sit at ~one frame (~20 ms) of queued audio.

6. **Unpause + stall recovery** — **`handle_v4_clock_sync` / `handle_clock_beacon`** call **`dashcdg_rx_rearm_live_video_after_unpause_locked`** plus **`dashcdg_rx_reprime_audio_after_host_underrun_locked`**. On **non-legacy** desktop builds, **dead backend**, **zero-buffer**, and **buffered-silent** auto-recovery use **`dashcdg_rx_rebuild_audio_decode_path_locked`** (same class of work as **`dashcdg_rx_configure_audio_locked`**: stop host stream, re-init ring, reopen device, re-prime jitter/decoders) so the RX does not stay silent until a manual **D** toggle.

## Enterprise group sync (multi-receiver)

Normative requirements, soak gates, and implementation order (residual phase: detrended spread, smoothed TX controller, RX leader scaling, DAC trim, etc.): **`docs/specs/enterprise-group-sync-spec.md`**, **`docs/test/enterprise-group-sync-test-plan.md`**, **`docs/plans/enterprise-group-sync-tranches.md`**. Older enterprise master-plan markdown is archived under **`docs/archive/enterprise-sync-masterplan-2026-04/`** (stub files keep old paths).

**Implemented in code (2026):** TX `dashcdg_tx_refresh_rx_measurements_locked` detrends per peer (buffer+host), EMA-smooths absolute and residual series (τ ≈ 3.5 s), publishes **pipeline** vs **residual** spread; `phase_warn` / `clock_noisy` / v4 clock_sync wire spread use **residual**; jsonl includes `pipeline_phase_spread_ms` and `residual_phase_spread_ms` (`phase_spread_ms` aliases residual for scripts). RX follower leader trim is capped by **residual** `sync_group_phase_spread_ms` from the wire: `min(200 ppm, 2 * spread_ms)` unless leader trim is smaller. Optional **`DASHCDG_RX_DAC_TRIM_MS`** nudges the queue servo target (±500 ms).

**Also:** RX exports **EMA-filtered** clock offset (~850 ms τ) on wire stats / jsonl (`clock_offset_ema_ms` path). PCM queue pressure allows **+40 ms** slack during the first **5 s** after session change or until audio servo enable. TX **defaults to group-sync MEASURE**, then auto-switches to **ACTIVE** after **10 s** with controllers or **45 s** idle (`--v4-group-sync=active` for immediate ACTIVE).

## Observability

**`dashcdg_rx_configure_audio_locked`** logs:

`[rx] audio: output ring (session_sr=… pa_open_request_hz=… wire_ch=… host_ch=… frame_ms=… preroll=… prof=… codec=…)`

**A/V timeline:** RX **`--rx-av-sync-log-ms N`** prints **`[rx-av-sync]`** lines. **`--rx-graphics-clock sender|dac`** switches multi-receiver vs local-heard lyrics.

## Key files (quick map)

| Area | Files |
|------|-------|
| Jitter / priming | `core/src/audio_jitter.c`, `core/src/cdg_batch_jitter.c` |
| RX wiring | `platform/desktop/src/app_rx.c` |
| NB / Opus / PCM DSP | `platform/desktop/src/pcm_rate_convert.c`, `opus_codec.c`, NB codec adapters |
| Sneakernet packaging | `scripts/build_windows_sneakernet_dist.sh` — **`RUN_P3_DISASM=1`** opt-in |

## Build / test

- **`make test`** — core tests; **`test-pcm-rate-convert`**, **`test-opus-roundtrip`**, **`test-nb-codec-adapters`**.
- After edits, rebuild **`desktop-rx`** / **`desktop-tx`**; dist zips **do not auto-update** — rerun packaging if you ship **`build/dist/...`**.

## If behaviour regresses

1. Confirm **fresh binaries** (timestamp / log lines).
2. Correlate **`output ring`** log repeats vs **`wait-preroll`** HUD.
3. For sync issues: **`clock_sync`**, **`playback_ms`** on chunks, **`publish_render_snapshot`** vs **`live_packets_applied`**.

## Embedded / ESP32

Firmware should inherit this **stabilized desktop v4 contract** — see **`docs/hardware/esp32-receiver-architecture.md`**, **`docs/specs/embedded-rx-audio-profile.md`**, and **`.cursor/plans/esp32_embedded_enterprise_plan_b3bda7b3.plan.md`** (desktop prerequisite **tr0a** satisfied for ongoing soak).

Desktop soak and operator guidance now explicitly include an external/network-mounted media lane in addition to repo-local media paths; keep this reflected in new validation notes.

**ESP-IDF environment (read order matters):**

| Priority | Host | What to run |
| --- | --- | --- |
| **1 — operator Windows (Espressif)** | Win10/11, `C:\Espressif\` install | **`Initialize-Idf.ps1`** — exact one-liner in **`## Agent execution contract`** above; then `cd platform\espidf\projects\dashcdg_badge` and **`idf.py build flash`** (or `idf.py build` / `idf.py flash` separately). **Not** `third_party` bootstrap unless operator says so. |
| **2 — repo clone / Unix / CI** | Git Bash, WSL, Linux, headless | One-time: `bash scripts/bootstrap_esp_idf.sh` → `third_party/esp-idf/`. Each shell: `source third_party/esp-idf/export.sh`. One-shot build: `bash scripts/build_esp32_freertos_platform.sh`. |
| Flash (bash path) | same as row 2 | `bash scripts/flash_esp32_freertos.sh COM6` (override port). |

Full narrative: **`docs/embedded/esp-idf-setup.md`**. If **`third_party/esp-idf/tools/idf.py` is missing** on a Path B machine, bootstrap first. Override IDF revision with `DASHCDG_ESP_IDF_REF` (see `scripts/bootstrap_esp_idf.sh`).

## ESP32 badge: build / flash

There are **two supported ways** to get a working `idf.py` environment; pick the one that matches your shell.

### Path A — Native Windows (PowerShell + Espressif install)

For `platform/espidf/projects/dashcdg_badge`, use the **Espressif Windows toolchain** via **`C:\Espressif\Initialize-Idf.ps1`** and **`powershell.exe`**. **Cursor’s default bash does not count** — IDF must run under native PowerShell or the **`.ps1`** wrappers in **`scripts\`** (see **Cursor IDE on Windows** above).

- **Activate ESP-IDF first** (operator-standard; same as execution contract):

```text
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoExit -File "C:\Espressif/Initialize-Idf.ps1" -IdfId esp-idf-20ee62e792ea89630ac6a777ab3ebc57
```

- **Direct in an IDF-initialized PowerShell (operator default):** working directory **`platform\espidf\projects\dashcdg_badge`**, then **`idf.py build`**, **`idf.py flash`**, **`idf.py build flash`**, or **`idf.py -p COM6 build flash`**.
- **Scripted build + flash** (clears MSYS env vars; calls **`Initialize-Idf.ps1`** internally):  
  `powershell.exe -ExecutionPolicy Bypass -NoProfile -File "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\scripts\idf_build_flash_badge.ps1"`  
  Optional **`-Port COM9`** if not **COM6**.
- **Build only:** open PowerShell with **`Initialize-Idf.ps1`** as above, **`cd`** to **`dashcdg_badge`**, run **`idf.py build`** (no separate wrapper required).
- Expected serial port in current setup:
  - `COM6` (update **`-Port`** if different).
- Optional debug automation:
  - `CONFIG_DASHCDG_BADGE_DEBUG_AUTO_LAUNCH_KARAOKE_AFTER_DHCP=y` auto-opens Karaoke after first DHCP once per boot; **does not override** karaoke audio/video decode toggles from settings / NVS.

If ESP-IDF activation reports `MSys/Mingw is not supported`, the session inherited Git Bash/MSYS environment context. For Path A, start a **fresh native PowerShell** (or invoke **`powershell.exe -File scripts\idf_build_flash_badge.ps1`** from repo root so MSYS vars are cleared). For **Path B** (below), stay in Git Bash and use the repo bootstrap + `export.sh` instead of Path A.

### Path B — Git Bash / WSL / Linux / CI / Cursor agent (repo `third_party/esp-idf`)

Use this when **not** using Path A (including **Cursor’s bash** on Windows). From repo root:

1. One-time: `bash scripts/bootstrap_esp_idf.sh`
2. Each new shell **or** before manual `idf.py`: `source third_party/esp-idf/export.sh`
3. Build: `bash scripts/build_esp32_freertos_platform.sh` **or** `idf.py -C platform/espidf/projects/dashcdg_badge build` after step 2.

Do **not** assume `idf.py` is on `PATH` without step 2 or the build script. Canonical write-up: **`docs/embedded/esp-idf-setup.md`**.

After flashing, run the automated UART cycle and summary:

- Full loop (Path A: PowerShell `Initialize-Idf.ps1` + stripped MSYS; Path B: run `bash scripts/build_esp32_freertos_platform.sh` first, then use `--skip-build-flash` here if you only need UART): `python scripts/esp32_badge_debug_cycle.py --iterations 1 --log-seconds 120 --port COM6`
- Capture only: `python scripts/esp32_badge_debug_cycle.py --skip-build-flash --iterations 1 --log-seconds 120 --port COM6`
- Optional guard when you expect a busy UART: `--min-lines 400` exits **2** if capture is nearly empty (wrong COM / asleep cable).
- `python scripts/esp32_badge_log_summary.py <log-file>` (counts `rx_listening`, `cdg_jitter_ring_full`, DHCP, etc.)

During automated validation, wait for DHCP (`sta ip:` marker), then launch the CDG app on-device before evaluating playout stability counters.

**Two-phase HW check:** (1) Karaoke Settings — video **on**, audio **on** — confirm HUD/stream OK and low `cdg_jitter_ring_full` in the summary. (2) Same screen — video **off**, audio **on** (saved to NVS) — HUD must **not** treat missing CDG heap as failure (audio-only uses chunk/frame counters).

### ESP32 badge LVGL (`platform/espidf/projects/dashcdg_badge/main/`)

- **User-visible strings must be 7-bit ASCII** in C string literals passed to LVGL labels, buttons, `snprintf` text, and modal copy. Do **not** use Unicode punctuation or symbols (middle dot, em dash, smart quotes, Unicode ellipsis, non-breaking space, etc.): the default **Montserrat** subset often has **no glyphs** for those code points, so text renders as **missing tofu** or blank gaps.
- **Icons and pictograms** must come from **`LV_SYMBOL_*`** (LVGL symbol font), **`lv_image`** assets, or other **explicit** graphics paths — not from Unicode emoji or arbitrary Unicode code points in strings.

### Desktop lifecycle and sync notes (current)

- Win32 GDI path should treat move/resize and close lifecycle as first-class runtime behavior (no stuck process on close, no heavy blit during size/move loops).
- Equal-rate group-sync correction should avoid fragile assumptions about runtime SRC availability; keep in-rate playout paths bit-transparent unless active sync correction is explicitly requested.

## User preferences (from rules)

High-quality prose; minimal drive-by refactors; prefer code citations with fenced `startLine:endLine:filepath` blocks in chat; execute commands locally rather than only suggesting them.

## Operator trust rule (must-follow)

This rule is a **subset** of **`## Agent execution contract (non-negotiable)`** above; both apply.

When the operator says a file/path exists (especially test assets used daily), do **not** claim it is missing after a single failed lookup. Re-check with multiple discovery methods (`Glob`, `rg`, and nearby directories / likely naming variants) and ask for the exact path only after those checks. Prefer: "I couldn't locate it yet; please share exact path" over "it isn't in the repo."

The same applies to **tools** (including **`idf.py`**): never claim the workflow is impossible without having run the **correct** activation from this file — on the operator’s Windows machine that is the **`C:\Espressif\Initialize-Idf.ps1`** one-liner (execution contract), **not** blindly running **`bootstrap_esp_idf.sh`**.
