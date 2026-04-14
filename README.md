# dashcdg

`dashcdg` is now structured as the foundation for a portable karaoke broadcast and receiver platform.

Today the repo contains:

- a portable CD+G core with deterministic seeking and keyframes
- a versioned UDP-friendly transport protocol
- a local desktop player
- a desktop multicast transmitter proof
- a desktop multicast receiver proof
- core and protocol tests
- architecture, protocol, hardware, and quality-gate documentation

## Repository layout

- `core/`: portable CD+G and clock logic
- `proto/`: versioned wire protocol
- `platform/desktop/`: desktop audio, file I/O, and OpenGL rendering
- `apps/desktop-player/`: local `MP3+G` playback
- `apps/desktop-tx/`: Wi-Fi multicast proof transmitter
- `apps/desktop-rx/`: Wi-Fi multicast proof receiver
- `docs/`: roadmap deliverables and tranche specs
- `tests/`: portable core and protocol tests

## Build

Build portable libs, tests, and the desktop transmitter:

```sh
make
```

Build and run the portable test suite:

```sh
make test
```

Build the desktop OpenGL applications as well:

```sh
make debug
```

## Desktop app usage

Local desktop player:

```sh
build/bin/desktop-player <file.cdg> [file.mp3]
```

If you pass only a `.cdg` file, the player runs in CDG-only validation mode using the native CD+G packet clock instead of audio playback. This is useful for graphics-only samples such as files in the local `cdg/` folder.

Desktop multicast transmitter:

```sh
build/bin/desktop-tx <multicast-address> <port> <song-id> <file.cdg> [warmup-ms]
```

Desktop multicast receiver:

```sh
build/bin/desktop-rx <multicast-address> <port> <local.mp3>
```

## Notes

- The OpenGL apps still require desktop graphics/audio libraries such as GLEW, GLUT, and PortAudio.
- The desktop TX/RX path is intentionally a proof tranche for protocol and synchronization, not a finished production venue stack.
- The ESP-IDF receiver and hardware program are documented in `docs/` and staged for future implementation.
