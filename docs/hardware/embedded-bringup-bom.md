# Embedded Bring-Up BOM

## Purpose

Define the minimum hardware needed to get the first embedded `dashcdg` receiver
moving quickly, with cheapest-first notes and a clear split between:

- the preferred all-in-one CYD path
- the cheapest self-contained fallback
- the lowest-cash reuse-an-existing-ESP32 path

This BOM is intentionally prototype-oriented. It is not a production enclosure
or final product bill of materials.

## Path A: Preferred all-in-one bring-up

Target:

- two `Freenove ESP32 CYD 3.2 inch` boards with `240x320` `IPS` `ST7789`
  panels

Minimum purchase list:

- `2x` `Freenove ESP32 CYD 3.2 inch`
- `2x` USB cables known to carry data
- `1x` small amplified speaker or powered desktop speaker for bench audio tests
- `1x` simple external digital-audio path if the chosen board revision does not
  already expose a usable audio output path

Recommended add-ons:

- `1x` `MAX98357A`-class `I2S` DAC/amp module or similar simple digital-audio
  board
- `1x` small `4 ohm` or `8 ohm` speaker
- `2x` `18650` cells only if you already have a safe way to mount and charge them
- `1x` protected `18650` holder or equivalent battery carrier
- `1x` charge/protection module if the board revision does not provide one
- jumper wire kit

Why this is the preferred path:

- lowest integration burden
- fastest route to firmware bring-up
- easy to keep one board stable while the second is used for riskier tests

Cheapest-first note:

- buy the two boards first
- delay battery hardware until USB-powered bring-up is stable
- delay audio hardware until the screen, Wi-Fi, and timing paths are alive unless
  your board revision already makes audio hookup trivial

## Path B: Cheapest self-contained fallback

Target:

- two `Freenove ESP32 CYD 2.8 inch` boards with `240x320` `TN` `ILI9341`
  panels

Minimum purchase list:

- `2x` `Freenove ESP32 CYD 2.8 inch`
- `2x` USB data cables
- the same optional external audio path listed for Path A if required by the
  chosen board revision

Use this path when:

- the `3.2 inch` `IPS` version is unavailable
- the price delta is large enough that it slows the project down

Cheapest-first note:

- keep the same firmware and validation plan as Path A
- do not change to a `320x480` board just because the `2.8 inch` version is
  cheaper

## Path C: Lowest cash-outlay fallback

Target:

- reuse an existing plain ESP32 board
- add a cheap `ST7789` display

Minimum purchase list:

- `1x` or `2x` cheap `ST7789` SPI display modules
- jumper wires
- breadboard or perfboard
- USB data cable for the ESP32 board
- simple external digital-audio path

Recommended audio parts:

- `1x` `I2S` DAC/amp module
- `1x` small speaker or powered speaker input

Advantages:

- lowest new spend if a working ESP32 board is already on hand

Disadvantages:

- slower bring-up
- more wiring mistakes to debug
- more variation across ESP32 dev boards

Use this only if:

- spend matters more than elapsed time to first picture

## Audio hardware direction

For the first prototype, the audio path should optimize for bring-up simplicity,
not fidelity.

Preferred order:

1. simple digital audio out from the MCU
2. cheap external `I2S` DAC/amp module
3. small speaker or powered speaker input

Avoid for the first milestone:

- designing a custom analog output stage
- battery-powered speaker integration before USB-powered validation exists
- treating touch audio controls as required

## Battery direction

Battery is useful, but it should not block first firmware proof.

Cheapest and fastest sequence:

1. bring up firmware on USB power
2. prove screen, Wi-Fi, and basic controls
3. add audio hardware
4. add battery hardware last

If battery is added early:

- use protected cells or a known-safe pack
- use a holder or carrier, not loose test wiring
- keep charging and discharge protection explicit

## Suggested order of purchase

If buying now:

1. `2x` `Freenove ESP32 CYD 3.2 inch`
2. verify both boards flash and boot over USB
3. add a simple `I2S` DAC/amp only if the board revision does not already solve
   the first audio test
4. add speaker and battery hardware after the first display/Wi-Fi milestone

## What not to buy yet

Do not front-load money into:

- enclosure parts
- high-capacity battery packs
- larger `320x480` display boards
- touch-specific accessories
- polished speaker hardware

Those should wait until the embedded validation matrix proves that the selected
board has enough headroom for the first transport, display, and audio goals.
