# Test plan: V4 network observability and sync (future)

## Scope

Validate **reporting**, **timing consistency**, and **safe adaptation** once the stats/adaptation features from [`v4-network-stats-and-adaptation.md`](../specs/v4-network-stats-and-adaptation.md) and [`v4-display-audio-sync.md`](../specs/v4-display-audio-sync.md) are implemented.

Until then, this checklist is a **placeholder** for QA and automation.

## Preconditions

- Two or more hosts on the same L2/L3 segment (or routed multicast where supported).  
- Known **v4** session: Opus (and optionally one narrowband codec).  
- Ability to inject **loss** (netem, switch ACL, Wi‑Fi distance) and **delay** (tc / Clumsy / WAN emulator).

## Cases (draft)

1. **Stats channel health**  
   - Receiver emits reports at configured interval; no media glitches attributable to stats traffic alone.

2. **Field sanity**  
   - `audio_buffer_ms` correlates with intentional buffer size changes.  
   - `jitter_*` rise under synthetic jitter; `loss_pct` tracks injected loss.

3. **Clock offset**  
   - `clock_offset_estimate_ms` stable within ±X ms on LAN; document X per platform.

4. **Adaptation (when implemented)**  
   - Increasing loss triggers **FEC** or **bitrate** change within bounded time.  
   - No **feedback oscillation** (hysteresis verified).

5. **Display–audio**  
   - RX: CDG frame index vs audio PTS within **one subcode frame** + buffer slack.  
   - TX preview: offset vs RX matches configured **preview delay** within tolerance.

6. **Retro / MCU**  
   - Stats path optional; **no crash** when disabled; minimal RAM/CPU when enabled.

## Automation

- Prefer **scripted** sender/receiver with golden thresholds; optional **CI** smoke without real multicast (loopback UDP).

## Exit criteria (TBD per release)

- Documented thresholds pass on **Windows amd64** baseline; optional **i686 retro** smoke.
