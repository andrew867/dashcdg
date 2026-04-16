# ESP32 Board Interface Notes

## Purpose

Capture the practical hardware-interface assumptions for the first embedded
receiver bring-up, without pretending every board revision is identical.

These notes are for two paths:

- CYD-style all-in-one ESP32 display boards
- a plain ESP32 board plus external display/audio hardware

## General rules

- confirm the exact board revision before locking pin assignments
- do not block firmware bring-up on battery integration
- treat touch as optional
- prefer buttons-first control paths
- prefer a simple digital audio path over custom analog output work

## CYD-style board path

Primary target:

- `3.2 inch 240x320 IPS ST7789` CYD

Fallback:

- `2.8 inch 240x320 ILI9341` CYD

### Display

Assume:

- integrated SPI display
- board-specific display reset, backlight, and SPI pin mapping
- need for one board-support layer per board family or exact revision

Bring-up rule:

- get a stable full-screen test working first
- optimize partial updates later if measurements require it

### Touch

Assume:

- touch controller and wiring vary by board family
- touch quality is not critical to the first receiver milestone

Bring-up rule:

- do not make touch a dependency for boot, join, or status display

### Buttons

Preferred first inputs:

- physical buttons already present on the board
- reset and boot buttons only for flashing and recovery unless additional use is
  straightforward

### Audio

Assume:

- some CYD revisions expose speaker or audio-friendly pins, some do not
- the exact onboard path must be verified from the purchased hardware

Preferred first audio direction:

- external digital-audio module if the board does not already make audio output
  easy

### Battery

Assume:

- battery and charge circuitry vary across display-board variants
- some revisions may be convenient for battery use, others may need external
  help

Bring-up rule:

- validate on USB power first
- only add battery once boot, display, Wi-Fi, and logging are stable

## Plain ESP32 fallback path

Use this path only when spend matters more than elapsed bring-up time.

### Display

Preferred display direction:

- common `ST7789` SPI module

Reason:

- cheap
- common
- good enough for the first proof

Bring-up rule:

- keep wiring short and obvious
- avoid stacking multiple peripherals before the display path is known-good

### Buttons

Preferred direction:

- two or three simple buttons on spare GPIOs

Suggested first actions:

- status/menu
- brightness or volume
- test action or recovery path

### Audio

Preferred direction:

- simple external `I2S` DAC/amp module

Avoid at first:

- custom analog output circuits
- battery-powered audio without a stable USB-powered baseline

### Battery

If battery is needed:

- use protected cells or a known-safe pack
- use a proper holder or carrier
- keep charge/protection explicit

## Software-facing implications

The board support layer should isolate:

- display init differences
- touch presence or absence
- button GPIO mapping
- backlight control if present
- audio-output wiring differences
- battery and charge telemetry if available

The application layer should not care whether the selected board is:

- `3.2 inch CYD`
- `2.8 inch CYD`
- plain ESP32 plus external modules

It should only consume abstract capabilities such as:

- display present
- buttons present
- touch optional
- audio output available
- battery telemetry available

## Practical recommendation

For the first receiver build:

1. implement the `3.2 inch CYD` board-support layer first
2. treat the `2.8 inch CYD` as a near-neighbor fallback
3. leave the plain ESP32 path as a lower-cost secondary hardware profile

That keeps firmware effort concentrated on the fastest path to first picture and
first audio without closing off a cheaper fallback later.
