# ESP32 Audio Feasibility Gate

## Goal

Determine whether synchronized on-wire `Opus` decode and audio playout can coexist with:

- Wi-Fi receive
- CD+G decode
- TFT rendering
- battery constraints

on the selected ESP32 hardware profile.

## Required measurements

- sustained Opus decode throughput at the chosen frame size and bitrate
- worst-case RAM usage with jitter buffers, bootstrap asset buffers, and render state
- SPI display bandwidth under lyric-heavy updates
- drift between audio playout clock and network session clock
- battery current draw during peak display + Wi-Fi + audio load

## Pass criteria

- no underruns during a full-song soak
- stable lyric sync within visibly acceptable bounds
- sufficient free RAM margin for packet jitter buffering and late-join bootstrap
- thermal and battery behavior inside device limits

## Failure outcomes

If the gate fails, keep protocol compatibility and move local audio to:

- an external venue audio path
- a companion processor
- a higher-tier MCU/SBC receiver profile
