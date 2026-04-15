# dashcdg

`dashcdg` is a portable karaoke broadcast/receiver codebase centered on:

- deterministic CD+G decode and seek
- a versioned UDP-friendly wire protocol
- desktop proof applications for local playback, multicast transmit, and multicast receive
- a live on-wire `Opus + timed CDG` proof path with late-join bootstrap

## Current repository contents

- `core/`: portable CD+G decode and simple remote/local clock discipline
- `proto/`: protocol v2 framing and packet serialization/parsing
- `platform/desktop/`: desktop OpenGL renderer, PortAudio plumbing, MP3 file playback, and Opus helpers
- `apps/desktop-player/`: local desktop player and entrypoint shim for TX/RX modes
- `docs/`: protocol, validation, architecture, hardware, and release docs
- `tests/`: portable CD+G, clock, and protocol tests

## Build

Default build:

```sh
make
```

Build desktop binaries and debug artifacts:

```sh
make debug
```

Run the portable test suite:

```sh
make test
```

Windows portable package:

```sh
make package
```

That produces `build/release/dashcdg-windows-portable.zip`.

## Desktop dependencies

The desktop apps require:

- OpenGL plus GLEW
- FreeGLUT
- PortAudio
- `libopus`

On the current Windows/MSYS2 flow, `mingw-w64-x86_64-opus` must be available so `desktop-tx`, `desktop-rx`, and `desktop-player` can link and ship `libopus-0.dll`.

## Desktop app usage

Local player:

```sh
build/bin/desktop-player [--shuffle] [<folder> | <file.cdg>|<file.mp3>|<file-stem> [file.mp3]]
```

Integrated network modes through the player entrypoint:

```sh
build/bin/desktop-player tx [--display] [endpoint-address] [port] [song-id] <file|folder> [warmup-ms]
build/bin/desktop-player rx [endpoint-address] [port]
```

Standalone network transmitter:

```sh
build/bin/desktop-tx [--display] [endpoint-address] [port] [song-id] <file|folder> [warmup-ms]
```

Standalone network receiver:

```sh
build/bin/desktop-rx [--headless] [endpoint-address] [port]
```

Network defaults:

- default endpoint address: `239.255.77.77`
- default UDP port: `24684`
- TX and RX still accept explicit multicast endpoints
- TX and RX now also accept IPv4 broadcast destinations such as `192.168.0.255` for `/24` LAN broadcast setups

## Media resolution behavior

- With no path, `desktop-player` scans the local `cdg/` folder and plays tracks sequentially.
- With `--shuffle`, `desktop-player` scans a folder and shuffles the playlist.
- With a folder path, TX/player scan for `.cdg` files and auto-pair same-name `.mp3` files.
- With a single `.cdg` file, the app still auto-detects a sibling `.mp3` when present.
- With a single `.mp3` file, the app looks for a same-name sibling `.cdg`.
- With a bare stem such as `C:/songs/MyTrack`, the app resolves `MyTrack.cdg` and `MyTrack.mp3`.
- With `<file.cdg> <file.mp3>`, the player uses the explicit pair.
- In TX, paired media is treated as `MP3+G`; CDG-only tracks fall back to graphics-only timing and omit network audio metadata.

## UI Controls

`desktop-player`:

- `Left Arrow`: seek backward by 1000 ms
- `Right Arrow`: seek forward by 1000 ms

`desktop-tx` foreground command controls:

- `p`: play/pause
- `n`: next track
- `b`: previous track
- `r`: restart current track
- `f`: force late-join asset rebroadcast
- `s`: print current status
- `v`: toggle preview visibility when `--display` is active
- `h` or `?`: print help
- `q`: quit

`desktop-tx --display` preview:

- opens a local OpenGL preview window while TX continues broadcasting
- the same `v` command blanks/unblanks only the local preview; it does not stop network send

`desktop-rx`:

- `--headless` prints status lines to stdout once per second and does not open a window
- GUI mode shows a HUD in the window
- `S` in the RX window prints the current status line to stdout

## Current networked proof behavior

The desktop TX/RX proof is currently a hybrid late-join/live-media transport:

- `ANNOUNCE`, `ASSET_CHUNK`, and `CLOCK_BEACON` keep the original late-join bootstrap working
- `AUDIO_FRAME` carries live Opus frames on the wire
- `CDG_BATCH` carries timed groups of CD+G subchannel packets on the wire
- `PTP_SYNC`, `PTP_FOLLOW_UP`, `PTP_DELAY_REQ`, and `PTP_DELAY_RESP` maintain a software-timestamped round-trip clock estimate
- RX decodes network Opus into a queue-driven PortAudio stream when audio metadata is present
- RX now uses bounded pending queues plus deadline-based skip logic for reordered or missing live audio/CD+G packets
- TX now emits bounded `FEC_PARITY` packets for audio and CD+G groups, and RX attempts single-missing-packet repair before treating a group as lost
- TX/RX status lines now expose startup gates, clock-update quality, repair hotness, and FEC profile/overhead telemetry
- desktop TX/RX now default to `239.255.77.77:24684`, while also allowing explicit multicast or IPv4 broadcast endpoints

## Impairment validation relay

For repeatable loss/reorder proof runs, use `scripts/desktop_impairment.py` between TX and RX instead of sending TX directly to the RX multicast group:

```sh
python scripts/desktop_impairment.py \
  --listen-group 239.255.77.91 \
  --listen-port 24684 \
  --emit-group 239.255.77.92 \
  --emit-port 24685 \
  --drop-every 11
```

The relay joins one multicast group, applies deterministic packet loss, reordering, and burst-loss rules, then forwards to a second multicast group while printing packet counters. See `docs/test/desktop-impairment-validation.md` for the baseline, single-loss, reorder, burst-loss, and mixed-impairment command matrix.

Current TX defaults:

- audio sample rate: 48 kHz
- channels: 2
- Opus frame size: 20 ms
- Opus bitrate target: 128 kbps
- announced playout delay: 500 ms
- announced audio FEC group size: 5
- announced CDG FEC group size: 9
- CDG packets per live batch: up to 6
- asset bootstrap chunk size: 1024 bytes

## Important current limitations

- The desktop proof now has a repeatable impairment relay and validation workflow, but long-duration impaired multicast soak data is still incomplete.
- Clock discipline now includes software-timestamped `PTP_DELAY_REQ` / `PTP_DELAY_RESP`, but it is still not a hardware-timestamped or sub-millisecond implementation.
- Late join is still guaranteed primarily by repeated asset replay, not by long-window FEC or retransmit.
- RX startup can still show a small number of early Opus decode failures or deadline skips during bring-up before the steady-state queue settles.
- This is a proof tranche, not a finished venue-grade transport stack.

## Related documentation

- `docs/specs/transport-protocol.md`: full protocol v2 field-level documentation
- `docs/test/desktop-proof-plan.md`: what the desktop proof is intended to prove
- `docs/test/desktop-impairment-validation.md`: repeatable impaired-network proof workflow and expected counters
- `docs/architecture/portable-core.md`: portable vs desktop-specific boundaries
- `docs/hardware/`: future ESP32 receiver and productization docs
- `docs/ops/quality-gates.md`: current tranche release criteria
