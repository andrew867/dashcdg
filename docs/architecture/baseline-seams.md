# Baseline Architecture, Timing, and Portability

## Current desktop playback model

The original player had the right synchronization nucleus for a broader platform, and the current desktop proof extends that model onto the wire:

1. audio playout is the time authority once the receiver has started steady-state playback
2. CD+G state is deterministic and can be replayed to an arbitrary packet index.
3. Rendering is a projection of `cdg_state` onto a platform surface.

That architecture is now preserved in a more explicit form:

- `core/include/dashcdg/cdg.h`: deterministic CD+G state, packet processing, keyframes, seeking.
- `core/include/dashcdg/media_clock.h`: portable monotonic clock and remote/local timeline discipline.
- `platform/desktop/include/dashcdg/desktop_audio.h`: desktop audio backend for local-file playback and queue-driven streaming playout.
- `platform/desktop/include/dashcdg/opus_codec.h`: desktop Opus encode/decode wrapper used by the live network proof.
- `platform/desktop/include/dashcdg/gl_renderer.h`: OpenGL renderer for palette-index framebuffer output.

## Timing model

- CD+G packets advance at `300` packets per second.
- The transport and local player both convert between milliseconds and packet counts using integer helpers in `core/include/dashcdg/common.h`.
- The receiver stack currently supports two practical timing sources:
  - `network audio playout clock`: when `AUDIO_FRAME` decoding and streaming playback are active.
  - `network-disciplined sender clock`: during startup, before playout begins, or while the receiver is still filling its live jitter queues.
- RX network mode now uses network audio only; local-file audio remains a local-player concern rather than a network-receiver fallback.
- The current desktop transport can run against either multicast endpoints or explicit IPv4 broadcast endpoints, but both modes share the same on-wire audio/CD+G/PTP behavior.

## Portability contract

The portable engine must not depend on:

- OpenGL
- PortAudio
- file I/O
- sockets
- GLUT event loops

Those concerns are isolated to the desktop platform layer or application entry points.

## CD+G state contract

The engine now tracks more than the legacy framebuffer:

- palette (`color_table[16]`)
- pixel indices (`framebuffer[300*216]`)
- smooth scroll offsets (`display_h_offset`, `display_v_offset`)
- transparency levels for all 16 palette entries

That state is sufficient to support desktop rendering and future MCU renderers without re-decoding the packet stream.

## Portability gaps still owned by platform layers

- socket APIs still use POSIX-style code in the current desktop TX/RX proof apps
- desktop OpenGL path still uses GLUT-era immediate rendering for simplicity
- ESP-IDF integration is specified but not yet implemented as a buildable target in this repo
