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

That portable set now reflects the desktop proof's protocol v2 direction:

- bootstrap asset packets for late join
- live `Opus` audio packet framing
- live timed `CDG_BATCH` framing
- basic sender clock tracking via sync/follow-up packets

### ESP-IDF-specific layers to implement next

- Wi-Fi transport adapter
- jitter buffer and playout scheduler
- Opus decode integration suited to the chosen ESP32 profile
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
- the same protocol session should eventually be consumable by desktop and ESP32 receivers
- the embedded receiver should preserve late join through bootstrap asset replay even before bounded FEC is implemented
- firmware must tolerate packet loss and late join without user intervention
- OTA and factory reset are part of the platform contract, not optional extras
