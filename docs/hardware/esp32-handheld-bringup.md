# ESP32 Handheld Bring-Up

## Purpose

Define the first embedded handheld bring-up path for `dashcdg` on the selected
ESP32 display board.

Primary target:

- `Freenove ESP32 CYD 3.2 inch`

Fallback target:

- `Freenove ESP32 CYD 2.8 inch`

The goal is not a polished handheld. The goal is to prove that one MCU-class
target can:

- join the session over Wi-Fi
- render meaningful on-screen status and video
- provide a simple local audio confirmation path
- expose enough observability to debug startup and sync

## Platform assumptions

The first reference platform stays aligned with
`docs/hardware/esp32-receiver-architecture.md`:

- ESP32-class MCU
- `ESP-IDF + FreeRTOS`
- small full-color SPI TFT
- simple controls
- battery support is useful, but not a day-one dependency

The first board-specific implementation should assume:

- USB-powered bring-up first
- buttons-first UI
- touch optional
- local audio is a feasibility gate, not a guaranteed permanent feature

## First firmware milestone

The first firmware milestone is complete when one board can:

1. boot into a known receiver app
2. join Wi-Fi reliably
3. show a loading or late-join screen quickly
4. parse enough transport metadata to expose session state
5. render useful visual progress before deep backfill completes
6. start a simple local low-bitrate audio path for sync and human validation
7. expose logs or on-screen counters that explain why startup is waiting

## Bring-up order

### Stage 0: Bare board sanity

Prove the board can:

- flash firmware over USB
- reboot cleanly
- print logs over serial
- light or toggle a simple visual indicator

Do not add battery or audio work yet.

### Stage 1: Display and controls

Bring up:

- panel init
- backlight control if present
- a simple full-screen color test
- a simple text/status screen
- at least one button input

Touch is optional at this stage.

Exit condition:

- the device can always display a stable boot/status page after reset

### Stage 2: Wi-Fi and session discovery

Bring up:

- saved Wi-Fi credentials or a fixed lab config
- UDP receive path
- enough packet parsing to detect a live session
- a status screen that shows connection state and packet counters

Suggested minimum states:

- `boot`
- `wifi-join`
- `wait-session`
- `wait-config`
- `wait-first-picture`
- `wait-first-audio`
- `ready`

Exit condition:

- the board can discover a live sender and show useful status without crashing

### Stage 3: Visual startup path

Bring up the smallest useful render path first:

- loading screen state
- compact visual anchor or snapshot path
- later live visual delta path

Important rule:

- first visible screen matters more than perfect asset rebuild

Exit condition:

- the board shows something useful quickly after joining
- the board can transition from loading to live visual state

### Stage 4: Timing and observability

Add:

- local monotonic timing
- sender/local timing observations needed by the chosen receiver path
- visible startup-state diagnostics
- packet, reorder, repair, and dropped-frame counters

Exit condition:

- startup failures are diagnosable without attaching a debugger to every run

### Stage 5: First local audio path

Add the simplest acceptable audio path for human validation:

- the low-bitrate resilience-oriented codec path first
- simple audio output through a cheap external digital-audio module if required
- enough buffering to avoid instant underruns during normal startup

This stage is about:

- confirming that audio exists
- checking rough sync against the visible lyrics/video path
- measuring whether the selected board has enough headroom

This stage is not about:

- final audio quality
- final speaker design
- production power behavior

Exit condition:

- a human can confirm that audio starts and remains close enough to the visual
  path to validate receiver behavior

### Stage 6: Battery and portability

Only after USB bring-up is stable:

- add battery input
- add charge/protection hardware if needed
- measure boot behavior on battery
- confirm brownout behavior is visible and recoverable

Exit condition:

- board behavior on battery is predictable enough for walk-around testing

## Controls policy

For the first prototype:

- buttons are sufficient
- touch is optional
- the receiver must not depend on touch to complete boot or join

Suggested first controls:

- one button for status or menu
- one button for brightness or volume
- one button for reset-to-defaults or test action if convenient

## Display policy

The display path should prefer the simplest architecture that can survive
measured lyric-heavy updates:

- start with full-screen uploads if that gets the first picture working quickly
- move to dirty-rectangle, banded, or tile-aware updates only if measurements
  prove they are needed

Do not over-design the renderer before basic throughput is measured on the real
board.

## Audio policy

The first audio target should follow the repo's current direction:

- keep `quality` mode as a higher-cost reference path where appropriate
- prioritize the simpler low-bitrate `resilience` style path for first embedded
  bring-up

If local audio fails the feasibility gate:

- keep the same transport/session behavior
- continue as a display-first receiver
- move audio validation to a companion path rather than stalling the whole board

## Battery policy

Battery is a secondary milestone.

Rules:

- do not block core bring-up on battery integration
- do not assume every CYD revision has the same battery or charge support
- prefer USB for early transport and render debugging

## Suggested software modules

The first firmware split should stay simple:

- board support
- display driver
- input handling
- Wi-Fi transport adapter
- packet parser
- startup-state controller
- visual render path
- simple audio output path
- telemetry and status output

## Bring-up checklist

The first prototype path should answer these questions in order:

1. Can the board flash, boot, and log?
2. Can the panel show a stable status screen?
3. Can the board join Wi-Fi and discover the sender?
4. Can it show a first useful visual state quickly?
5. Can it expose why startup is waiting?
6. Can it produce simple local audio without collapsing render or Wi-Fi?
7. Can it stay stable long enough for a full-song validation run?

## Non-goals for the first pass

Do not treat these as blockers for the first milestone:

- polished enclosure work
- touch-first UX
- final battery life
- final audio fidelity
- final OTA behavior
- production-grade provisioning

Those belong after the board proves that the embedded receiver path itself is
worth pushing forward.
