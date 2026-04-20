# ESP-IDF Receiver Architecture

## Reference target

First reference device:

- ESP32-class MCU running ESP-IDF
- small full-color SPI TFT
- battery + charge path
- USB-C for charging and firmware
- simple user input: buttons or rotary encoder

## Software layers

### Portable layers reused from desktop

- protocol framing
- clock discipline logic
- CD+G decoder
- keyframe and seek model

That portable set should track the desktop proof's **protocol v4** direction:

- bootstrap asset packets for late join
- v4 session info and narrowband audio chunks
- live **`Opus`** audio (optional on MCU; requires libopus-class integration)
- narrowband **`audio_codec_id` 2–7:** on **desktop**, each id maps to its **native** payload path (NB-IMA, QCELP, EVRC, AMR-NB/WB, Bluetooth SBC per [`../specs/v4-audio-codecs.md`](../specs/v4-audio-codecs.md)), with DSP/resample glue in **`platform/desktop`**. On **ESP32**, the **first-party fixed-point** story remains **`dashcdg_nb_ima_*`** (**id 2**) until vendor decode ports land; higher ids may be decode-later milestones per [`../specs/audio-codec-modules.md`](../specs/audio-codec-modules.md).
- live timed `CDG_BATCH` framing
- bounded `CDG_SNAPSHOT` framing for fast visual bootstrap/recovery
- software-timestamped `PTP_SYNC` / `PTP_FOLLOW_UP` / `PTP_DELAY_REQ` / `PTP_DELAY_RESP` clock discipline
- bounded `FEC_PARITY` framing for single-loss repair groups in the current desktop proof

### ESP-IDF-specific layers to implement next

- Wi-Fi transport adapter
- jitter buffer and playout scheduler aligned with **desktop v4** priming / skip policy (`core/` + [`AGENTS.md`](../../AGENTS.md) handoff)
- NB-IMA decode integration first; optional Opus decode if the board has headroom
- display driver adapter
- persistent settings
- power and battery telemetry
- OTA update integration
- input abstraction

## Renderer considerations

The MCU renderer must choose one of:

- full framebuffer upload
- dirty rectangle upload
- banded scanline upload
- tile-aware raster upload

That choice should be made from measured SPI throughput, not guesswork.

## Product assumptions

- display-first receiver is still acceptable before onboard audio is proven on target hardware
- the same protocol session should eventually be consumable by desktop and ESP32 receivers, regardless of whether the desktop sender is using multicast or IPv4 broadcast
- the embedded receiver should preserve late join through snapshot plus bootstrap asset replay even before embedded bounded FEC support is implemented
- firmware must tolerate packet loss and late join without user intervention
- OTA and factory reset are part of the platform contract, not optional extras
