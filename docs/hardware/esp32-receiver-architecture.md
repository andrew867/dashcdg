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

### ESP-IDF-specific layers to implement next

- Wi-Fi transport adapter
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

- display-first receiver is acceptable before onboard audio
- same protocol session should be consumable by desktop and ESP32 receivers
- firmware must tolerate packet loss and late join without user intervention
- OTA and factory reset are part of the platform contract, not optional extras
