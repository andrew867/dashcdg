# Amazon Board Recommendation

## Goal

Pick the cheapest practical non-Linux hardware path for the first embedded
`dashcdg` receiver, while keeping the first milestone realistic:

- Wi-Fi receive
- transport/session parsing
- useful on-screen status and video
- low-bitrate local audio for sync and human validation
- `ESP-IDF + FreeRTOS`, not Linux

## Current recommendation

Primary target:

- buy two `Freenove ESP32 CYD 3.2 inch` boards with the `240x320` `IPS`
  `ST7789` display

Why this is the current best first target:

- it keeps the panel workload in the `240x320` class instead of jumping to
  `320x480`
- it gives a physically larger display than the `2.8 inch` board without
  doubling the number of pixels the MCU must push
- the `IPS` panel is a better fit for handheld viewing than the `TN` options
- it stays in the `ESP32 + ESP-IDF` lane described by
  `docs/hardware/esp32-receiver-architecture.md`
- it is an all-in-one board, which shortens bring-up time compared with wiring a
  loose MCU, display, buttons, power path, and audio parts together

Ordering two of the same board is intentional:

- one can stay on stable bring-up firmware
- one can move faster on transport, codec, and UI experiments
- hardware faults or accidental pin conflicts do not stop all progress

## Why `240x320` is the sweet spot

`dashcdg` currently works from a `300x216` CD+G framebuffer on the desktop
path. For a first MCU receiver, the main hardware risk is not nominal panel
refresh marketing, but how much SPI display traffic the target must sustain
while it also handles Wi-Fi, decode, timing, and controls.

For that reason:

- `240x320` is the best practical first target class
- `320x480` is not a better workload match for the first prototype
- a larger physical `240x320` panel is more useful here than a denser `320x480`
  panel

## Ranked options

### 1. Best first prototype

`Freenove ESP32 CYD 3.2 inch`, `240x320`, `IPS`, `ST7789`

Strengths:

- best balance of readability, pixel load, and bring-up simplicity
- dual-core ESP32 class MCU is the safest fit for `Wi-Fi + render + simple
  audio`
- common display controller with broad example support
- good first board for `ESP-IDF`

Weaknesses:

- usually costs more than the `2.8 inch` board
- touch should still be treated as optional during early firmware work

### 2. Best cheaper all-in-one fallback

`Freenove ESP32 CYD 2.8 inch`, `240x320`, `TN`, `ILI9341`

Strengths:

- same practical pixel workload as the `3.2 inch` board
- likely the cheapest self-contained option that still makes sense
- still valid if the `3.2 inch` IPS board is expensive or unavailable

Weaknesses:

- `TN` is a worse fit than `IPS` for viewing angle and perceived quality
- physically smaller screen for lyrics and status text

### 3. Cheapest cash-outlay path

Reuse an existing plain ESP32 board and add a cheap SPI display.

Best use of this path:

- only if minimizing spend matters more than minimizing wiring and bring-up time

Preferred display direction for that path:

- cheap `ST7789` module first
- buttons-first UI
- simple external audio output path

Weaknesses:

- slower bring-up
- more bench wiring
- more board-specific uncertainty

### 4. Not preferred for the first prototype

`3.5 inch` or `4.0 inch` CYD-style boards with `320x480` `TN` `ST7796`
panels

Why they are not the first choice:

- they move to a much heavier pixel workload
- they do not improve the core firmware proof enough to justify that cost
- `TN` is still a compromise on viewing quality

Use them only if:

- larger physical screen area matters more than shorter bring-up time

### 5. Not preferred for the first onboard-audio prototype

`Pi Pico W` plus add-on display

Why:

- once the display is added, the total cost is no longer compelling enough
- it increases integration work
- it is a weaker first platform for the current `Wi-Fi + display + low-bitrate
  audio` target

### 6. Not preferred as the main target

`Waveshare ESP32-C6 1.47 inch` board

Why:

- too small for the first handheld receiver proof
- single-core class target is less comfortable for simultaneous network,
  display, and audio experiments
- better suited to small dashboards than the first end-to-end handheld proof

## Decision table

| Option | Good fit for first prototype | Main benefit | Main drawback |
| --- | --- | --- | --- |
| `3.2 inch CYD IPS ST7789` | Yes | Best overall balance | Costs a bit more |
| `2.8 inch CYD TN ILI9341` | Yes | Cheapest all-in-one | Smaller TN screen |
| Existing ESP32 + SPI LCD | Yes, but slower | Lowest cash outlay | More wiring risk |
| `3.5/4.0 inch 320x480 ST7796` | Not first choice | Bigger panel | Heavier display workload |
| `Pi Pico W + display` | No | Familiar ecosystem | More integration work |
| `ESP32-C6 1.47` | No | Small and cheap | Too constrained for first proof |

## Purchase recommendation

Recommended order now:

- two `Freenove ESP32 CYD 3.2 inch` boards

Fallback if price or stock changes:

- two `Freenove ESP32 CYD 2.8 inch` boards

Do not switch to the `320x480` boards just to get a larger screen unless later
measurements show the embedded display path has enough margin to justify the
extra pixel load.
