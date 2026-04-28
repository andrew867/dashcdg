# CD+G network hazard profile (ESP32 / Wi-Fi v4)

## Scope

Some MP3+G assets look fine on wired desktop RX but **visually explode** on ESP32 over Wi-Fi: smeared tiles, “snow”, or graphics that never recover until the next full-screen clear or v4 RLE anchor. That is usually **not** “missing clears” in the file — it is **lost or reordered subchannel PACKs** on the wire combined with **XOR tile** semantics and **sparse full-screen keyframes**.

Example asset in-tree: `cdg/Dev - In The Dark [AS Karaoke].cdg` (MP3+G sibling beside it). Profile with the tool below and compare numbers to other tracks under `cdg/` that behave well on the badge.

## How v4 CDG fails over lossy Wi-Fi

1. **Deltas are cumulative** — Each `V4_VIDEO_DELTA` carries raw CD+G PACKs. `TILE_BLOCK` overwrites pixels; `TILE_BLOCK_XOR` XORs into the framebuffer. One **wrong or dropped** XOR packet leaves **persistent garbage** until the next **authoritative reset** of that region (typically `MEMORY_PRESET` with `repeat==0` for full clear, or a **v4 RLE canvas anchor** that replaces the whole canvas).

2. **Keyframes in this codebase** — `dashcdg_cdg_reader_build_keyframes()` records a keyframe only on **`MEMORY_PRESET` with `repeat & 0x0F == 0`** (full-screen clear + captured palette/offsets). Long gaps between those clears mean **long XOR chains** with no cheap reset.

3. **Periodic anchors on TX** — Desktop TX emits RLE anchors on a timer (`DASHCDG_V4_VIDEO_ANCHOR_INTERVAL_MS`, **1000 ms** in `app_tx.c` at time of writing). If Wi-Fi loss is heavy, **one second** of XOR/tile corruption can still be visible between anchors.

4. **ESP32 receiver** — `badge_rx` applies anchors when valid; the first anchor is always accepted to recover intro/title quirks (`badge_rx_should_apply_v4_anchor`). After that, anchors must be **not behind** the jitter cursor. Bursty loss can still leave the canvas wrong until the next anchor or clear.

## Offline profiling tool

Build (debug bundle or explicit target):

```bash
make cdg-network-profile
```

Run on the `.cdg` next to the MP3:

```bash
./build/amd64/bin/cdg-network-profile "cdg/Dev - In The Dark [AS Karaoke].cdg"
```

It applies the **same subchannel trim** heuristic as TX (`dashcdg_cdg_compute_subchannel_trims`), then prints:

| Field | Meaning |
| --- | --- |
| `keyframes_memory_clear_repeat0` | Count of full-screen clears (same as internal keyframe list length). |
| `max_ticks_between_keyframes` | Longest gap between clears in **300 Hz** ticks (divide by 300 for seconds). Large ⇒ long error-propagation window. |
| `tile_block_xor` / `xor_pct` | XOR-heavy authoring ⇒ **more damage per lost packet**. |
| `leading_non_graphics_ticks_before_first_0x09` | Non-graphics PACKs before first `command==0x09` (see `cdg.c` seek comments; long runs are normal for some rips). |

**Heuristic:** tracks with **high `xor_pct`** and **large `max_ticks_between_keyframes`** (and/or very **low** `keyframes_memory_clear_repeat0` per minute) are **hostile** on lossy links unless FEC + anchors keep up.

## Mitigations (no asset re-authoring)

1. **More CDG FEC on TX** — Raise CDG repair level / redundancy so XOR/copy tiles survive the Wi-Fi leg (`desktop-tx` CDG FEC controls; adaptive path in `app_tx.c`). This is the first knob for “one bad burst trashes the canvas.”

2. **Shorter RLE anchor interval (TX code change)** — Lower `DASHCDG_V4_VIDEO_ANCHOR_INTERVAL_MS` from **1000 ms** if you accept more airtime and CPU on TX for faster self-heal on bad RF.

3. **Better RF / less contention** — 2.4 GHz + TCP background + multicast can starve small UDP; 5 GHz AP, shorter path, or dedicated SSID often matters more than codec tweaks.

4. **Badge join / late join** — Ensure the first **full** anchor completes after join (chunk pacing + FEC); logs `v4 anchor applied` vs `RLE invalid` on the badge help distinguish “loss” vs “bad anchor bytes.”

5. **Asset-side (optional re-mux)** — Re-exporting the CDG with a tool that inserts extra **full-screen clears** or reduces XOR authoring lowers hazard scores; that is outside this repo but is the deterministic fix if one publisher’s encoder is pathological.

## Related code

- Keyframe definition: `dashcdg_cdg_reader_build_keyframes` — `core/src/cdg.c`
- XOR / clear semantics: `dashcdg_cdg_state_process_packet` — `core/src/cdg.c`
- Anchor cadence: `DASHCDG_V4_VIDEO_ANCHOR_INTERVAL_MS` — `platform/desktop/src/app_tx.c`
- Badge anchor policy: `badge_rx_should_apply_v4_anchor` — `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
