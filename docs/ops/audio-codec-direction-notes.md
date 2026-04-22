# Audio codec direction notes

This note records operator-facing codec decisions that came out of live desktop soak testing so they do not get lost between code changes and roadmap work.

## Runtime note: EVRC retired from the active desktop path

- The old **EVRC** operator/runtime path on wire id **4** has been retired from normal desktop use.
- In the current runtime, wire id **4** is now treated as **`qcelp8k`**: a lower-rate QCELP mode built from the existing desktop QCELP adapter.
- TX still accepts legacy operator inputs such as `evrc` / `--badnet-v4-evrc` as compatibility aliases, but they now map to the id-4 **QCELP8K** path rather than the old EVRC encoder.
- Reason: repeated live testing showed EVRC was unstable under codec switching, could wedge TX into `audio_queue_starve`, and was not a good operator-facing choice even before the newer switch/cadence work.

## Current practical operator codec set

- `opus` for quality mode
- `amr-wb` for the default resilience mode
- `qcelp8k` for the low-rate “funny but usable” legacy-cell path
- `celp13k` for the higher-rate QCELP option
- `bluetooth-sbc` and `sbc-like` for additional resilience experiments

## Funny / weird codec backlog

These are intentionally non-essential and mostly here because supporting them in a multicast karaoke app would be funny:

- **iLBC**: classic VoIP brick-radio sound
- **G.722**: old telecom / broadcast wideband speech codec
- **Codec2**: ham-radio ultra-low-bitrate speech, extremely funny for karaoke
- **AC-3 / E-AC-3**: absurd in the opposite direction, because Dolby-style cinema audio in multicast karaoke is ridiculous

## Follow-up expectation

- Keep the user-facing docs and help text aligned to `qcelp8k` for wire id **4**
- Treat any remaining EVRC-specific code as research / compatibility baggage unless a future test plan explicitly revives it
