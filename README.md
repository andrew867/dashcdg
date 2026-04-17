# dashcdg

`dashcdg` is a karaoke transport and receiver proof codebase centered on:

- deterministic CD+G decode, replay, and seek
- a versioned UDP-friendly wire protocol
- desktop TX/RX proof apps for multicast and IPv4 broadcast transport
- live on-wire `Opus + timed CDG` playout with late-join bootstrap, bounded FEC, and periodic CDG state keyframes

The current desktop TX already sends audio and CD+G in parallel on the wire.
Ongoing work includes TX-side CD+G memory slimdown; **Windows** packaging now
includes a **Win32 GDI** receiver (`desktop-gdi-rx.exe`) and an optional
**retro** bundle without OpenGL/Opus — see `docs/specs/desktop-platform-support.md`.

## What Exists Today

The active desktop proof is a hybrid transport:

- `ANNOUNCE`, `ASSET_CHUNK`, and `CLOCK_BEACON` keep late-join/bootstrap behavior working
- `AUDIO_FRAME` carries live Opus audio
- `CDG_BATCH` carries live timed CD+G packets
- `CDG_SNAPSHOT` carries bounded framebuffer/palette state keyframes
- `PTP_SYNC`, `PTP_FOLLOW_UP`, `PTP_DELAY_REQ`, and `PTP_DELAY_RESP` provide a software-timestamped round-trip clock estimate
- `FEC_PARITY` provides bounded single-loss XOR recovery for short audio and CD+G groups

That means the receiver can:

- start from repeated asset replay like the original proof
- switch to live audio/CD+G before the full asset is rebuilt
- use snapshots for late join, pause-screen recovery, and mid-session visual re-anchoring
- keep explicit HUD/headless observability for playout gates, clock quality, and repair state

## Repository Map

- `core/`: portable CD+G decode and reusable clock-discipline helpers
- `proto/`: protocol v3 framing, serializers, and parsers
- `platform/desktop/`: desktop renderer, PortAudio streaming layer, Opus helpers, and TX/RX app logic
- `apps/desktop-player/`: local player plus entry shim for `tx` and `rx` modes
- `docs/`: architecture, protocol, proof, hardware, ops, and `docs/archive/` for superseded notes
- `tests/`: portable core, protocol, and helper tests

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
scripts/build_release.sh x64
```

That produces `build/amd64/release/dashcdg-windows-x64-portable.zip`.

Windows `x86` portable package:

```sh
scripts/build_release.sh x86
```

That produces `build/x86/release/dashcdg-windows-x86-portable.zip`.

MSYS2 builds use separate trees (`build/amd64/...` vs `build/x86/...`) so switching `MINGW_ARCH` does not reuse object files from the other architecture.

Build both Windows packages in one pass:

```sh
scripts/build_release.sh all
```

That also copies both zips into `build/dist/` for a single pick-up directory.
For an **XP-oriented PE** (subsystem/OS version fields and `WINVER`), set
`DASHCDG_WINDOWS_LEGACY=1` for the script or `WINDOWS_LEGACY_TARGET=1` for
`make` (see `docs/specs/windows-legacy-mingw-build.md`).

Or from the Makefile:

```sh
make dist-windows
```

For a **single USB-ready folder** (x64 + x86 + legacy P3 + retro layouts with
`README.txt`), run `make dist-windows-sneakernet` — output is
`build/dist/dashcdg-windows-sneakernet/` and `build/dist/dashcdg-windows-sneakernet.zip`
(see `docs/specs/windows-legacy-mingw-build.md`).

## Desktop dependencies

**Full GL stack** (`desktop-tx`, `desktop-rx`, `desktop-player`) requires:

- OpenGL plus GLEW
- FreeGLUT
- PortAudio
- `libopus`

**GDI-only receiver** (`desktop-gdi-rx.exe`, Windows): PortAudio, Opus, Win32
GDI/user32 — **no** FreeGLUT/GLEW in that link (see
`docs/specs/win32-gdi-view-backend.md`).

**Retro bundle** (`desktop-retro-*.exe`, `WINDOWS_RETRO_BUNDLE=1`): GDI only, **no** OpenGL; **Opus + PortAudio** via the same PIII-safe DLLs as other mingw32 builds (see `docs/specs/vendored-opus-portaudio-windows.md`). Narrowband codecs remain available via v4 session / `c` key.

**Windows desktop timing (TX/RX):** builds import only **`WINMM.dll`** for `timeBeginPeriod(1)`; on Vista+, `avrt.dll` (MMCSS) is **loaded at runtime** if present — static links to AVRT were removed so **Windows XP / 2000** EXEs start without “AVRT.dll not found”. See `platform/desktop/src/win32_timing_boost.c`.

On the current Windows/MSYS2 flow:

- `x64` packaging requires `mingw-w64-x86_64-gcc`, `mingw-w64-x86_64-opus`, `mingw-w64-x86_64-portaudio`, `mingw-w64-x86_64-freeglut`, and `mingw-w64-x86_64-glew`
- `x86` packaging requires `mingw-w64-i686-gcc`, `mingw-w64-i686-opus`, `mingw-w64-i686-portaudio`, `mingw-w64-i686-freeglut`, and `mingw-w64-i686-glew`
- **32-bit (mingw32) packages** use **PIII / pre-SSE2–safe** `libopus-0.dll` and `libportaudio.dll` built into `build/mingw32-p3-vendor/` by `scripts/build_mingw32_p3_opus_portaudio_shared.sh` (invoked from `scripts/build_release.sh` / sneakernet). Fetch upstream sources first: `make vendor-audio-sources`. Set **`SKIP_MINGW32_P3_VENDOR=1`** to reuse an existing vendor tree. **64-bit (mingw64)** still ships MSYS2 codec DLLs from the prefix.
- Standard portable Windows zips also include `glew32.dll`, `libfreeglut.dll`, `libwinpthread-1.dll`, a matching `libgcc_s_*.dll`, and `libstdc++-6.dll`. **V5+ roadmap** (multistream/adaptation placeholders): `docs/specs/v5-multistream-adaptation-architecture.md`, `DASHCDG_PROTOCOL_VERSION_V5` in `proto/include/dashcdg/protocol.h` (not on wire yet).

## Desktop App Usage

Local player (POSIX: `build/bin/...`; MSYS2 x64: `build/amd64/bin/...`; MSYS2 x86: `build/x86/bin/...`):

```sh
build/bin/desktop-player [--shuffle] [<folder> | <file.cdg>|<file.mp3>|<file-stem> [file.mp3]]
```

Integrated network modes through the player entrypoint:

```sh
build/bin/desktop-player tx [--help] [--headless] [--v3] [--audio-profile=quality|resilience] [--v4-audio-codec=<name>] [--badnet-v4|--badnet-v4-sbc|--badnet-v4-evrc] [endpoint-address] [port] [song-id] [file|folder] [warmup-ms]
build/bin/desktop-player rx [--help] [--headless] [--gdi] [endpoint-address] [port]
```

Standalone network transmitter (Windows MSYS2: **headless** `desktop-tx.exe`; use **`desktop-gdi-tx.exe`** for a GDI preview window without GL):

```sh
build/bin/desktop-tx [--help] [--v3] [--audio-profile=quality|resilience] [--v4-audio-codec=<name>] [--badnet-v4|--badnet-v4-sbc|--badnet-v4-evrc] [endpoint-address] [port] [song-id] [file|folder] [warmup-ms]
```

**V4 audio defaults (non-retro builds):** protocol v4 with **resilience** profile and **AMR-WB** (`--v4-audio-codec=amr-wb`) for wideband speech/music at 48 kHz session timing. **`--audio-profile=resilience`** adjusts the resilience/FEC profile only and does **not** switch the codec away from AMR-WB. Use **`--audio-profile=quality`** for Opus, or **`--v4-audio-codec=...`** to pick another id. Run **`--help`** on `tx` / `rx` for the full flag list. With a TTY, press **`c`** while TX is running to cycle codecs; receivers follow **`v4_session_info`** and reconfigure decoders automatically.

On Linux/macOS, `desktop-tx` is still the GL-capable binary: add **`--display`** for a FreeGLUT preview, or use **`desktop-player tx`** (preview on by default when the executable name contains `player`).

Standalone network receiver:

```sh
build/bin/desktop-rx [--help] [--headless] [--gdi|--win-gdi] [endpoint-address] [port]
```

Windows MSYS2 `make debug` also produces **`desktop-gdi-rx.exe`** and **`desktop-gdi-tx.exe`** (no GL in those links). On Windows, `desktop-rx` tries OpenGL first and **falls back to GDI** if the GL renderer fails to init; `--gdi` forces GDI.

Network defaults:

- default endpoint address: `239.255.77.77`
- default UDP port: `24684`
- default TX media library: `.\cdg` when no source path is provided
- TX and RX still accept explicit multicast endpoints
- TX and RX now also accept IPv4 broadcast destinations such as `192.168.0.255` for `/24` LAN broadcast setups

## Media Resolution Behavior

- With no path, `desktop-player` scans the local `cdg/` folder and plays tracks sequentially.
- With `--shuffle`, `desktop-player` scans a folder and shuffles the playlist.
- With no TX source path, `desktop-player tx` and `desktop-tx` scan the local `cdg/` folder.
- TX directory playback is shuffled on startup and reshuffled again when the playlist wraps.
- With a folder path, TX/player scan for `.cdg` files and auto-pair same-name `.mp3` files.
- With a single `.cdg` file, the app still auto-detects a sibling `.mp3` when present.
- With a single `.mp3` file, the app looks for a same-name sibling `.cdg`.
- With a bare stem such as `C:/songs/MyTrack`, the app resolves `MyTrack.cdg` and `MyTrack.mp3`.
- With `<file.cdg> <file.mp3>`, the player uses the explicit pair.
- In TX, paired media is treated as `MP3+G`; CDG-only tracks fall back to graphics-only timing and omit network audio metadata.
- TX reshuffles the playlist each time it wraps past the end, so repeated library loops do not stay in the same song order.

## UI Controls

`desktop-player`:

- `Left Arrow`: seek backward by 1000 ms
- `Right Arrow`: seek forward by 1000 ms

`desktop-tx` foreground command controls:

- `Space` or `p`: play/pause
- `n` or `]`: next track
- `b` or `[`: back through played-track history
- `r`: restart current track
- `f`: force late-join asset rebroadcast
- `c`: cycle v4 audio codec (re-sends session_info so receivers retune)
- `s`: print current status
- `v`: toggle preview visibility when a preview window is active
- `h` or `?`: print help
- `q`: quit

`desktop-player tx` / `desktop-tx --display` (non-Windows) / `desktop-gdi-tx.exe` preview:

- opens a local preview window (OpenGL or GDI) while TX continues broadcasting
- `Right Arrow`: next track
- `Left Arrow`: back through played-track history
- the same `v` command blanks/unblanks only the local preview; it does not stop network send

`desktop-rx` / `desktop-gdi-rx`:

- `--headless` prints status lines to stdout once per second and does not open a window
- GUI mode shows a HUD in the window (OpenGL + GLUT, or Win32 GDI per binary / `--win-gdi`)
- `S` in the RX window prints the current status line to stdout

## Desktop Streaming Architecture

```mermaid
flowchart LR
    src[CDG plus MP3 library] --> txprep[TX track load plus runtime queues]
    txprep --> ann[ANNOUNCE and CLOCK_BEACON]
    txprep --> ach[AUDIO_FRAME]
    txprep --> cb[CDG_BATCH]
    txprep --> snap[CDG_SNAPSHOT]
    ach --> fec[FEC_PARITY]
    cb --> fec
    ann --> rxstate[RX session state]
    snap --> rxstate
    fec --> rxjit[RX jitter and repair]
    ach --> rxjit
    cb --> rxjit
    rxjit --> rxaud[Opus decode plus PortAudio queue]
    rxjit --> rxcdg[Live CDG state]
    rxaud --> render[OpenGL or GDI follows playout clock]
    rxcdg --> render
```

Key runtime rules:

- TX now produces Opus frames incrementally on a dedicated audio thread during
  live send
- TX now uses a random-access CDG source for wire send paths; headless/default TX
  can stay file-backed, while preview mode still uses an in-memory fallback
- TX uses **protocol v4** on the wire by default (session info, loading screens,
  anchors, bounded startup pacing). Pass **`--v3`** to force the legacy v3-only sender loop.
- TX v4 supports `--audio-profile=quality|resilience` (Opus vs **NB-IMA** narrowband,
  same family as legacy “SBC-like” on the wire) plus **`--v4-audio-codec=`** and
  **`--badnet-v4*`** (see [`docs/specs/v4-audio-codecs.md`](docs/specs/v4-audio-codecs.md));
  narrowband codec implementation lives in **`core/src/nb_ima_codec.c`** (fixed-point, MCU-friendly).
  Optional **vendored** AMR / EVRC / QCELP / Bluetooth-SBC trees live under **`audio_modules/`**
  (see [`docs/specs/audio-codec-modules.md`](docs/specs/audio-codec-modules.md); run **`scripts/fetch_audio_codec_vendors.sh`** to populate `vendor/`)
- TX defaults to a `1000 ms` warmup before a new session starts
- TX network audio currently defaults to mono `48 kHz`, `20 ms` Opus frames, and `80 kbps`
- RX treats fresh `ANNOUNCE` packets as session re-anchors and rejects stale delayed PTP exchanges after track switches
- RX late-skip decisions now compare against sender playback time rather than sender wall clock, which keeps long playout from collapsing into false `wait-preroll`
- once audio playout is running, RX uses audio time as the render master
- pause mode freezes the song timeline, suppresses normal media scheduling, and repeatedly emits a custom pause-screen snapshot state
- snapshots are no longer just bootstrap helpers; RX can use them as active recovery anchors during a running session

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

## Current TX/RX Defaults

- audio sample rate: 48 kHz
- channels: 1 on TX network send path
- Opus frame size: 20 ms
- Opus bitrate target: 80 kbps
- announced playout delay: 500 ms
- announced audio FEC group size: 5
- announced CDG FEC group size: 9
- CDG packets per live batch: up to 6
- asset bootstrap chunk size: 1024 bytes
- TX warmup before a new session: 1000 ms
- pause-screen snapshot interval: 1000 ms

## Current Status

Implemented and documented:

- protocol v3 live-media packet families
- threaded TX/RX runtime boundaries with bounded queues between network, media, audio, and render work
- incremental TX MP3 decode/resample/Opus production during live send
- queue-driven RX streaming audio path
- timed live CD+G batches
- bounded XOR FEC generation and single-loss recovery
- periodic CDG snapshots for late join and recovery anchors
- TX pause/play with a networked pause screen
- default multicast plus explicit broadcast endpoint support
- startup, repair, and clock telemetry in both TX and RX
- measured isolated soak proving steady-state RX playout progression no longer falls back into `wait-preroll` while traffic continues
- measured forced session restart to a different track proving RX can tear down the old session and lock onto a fresh announce/bootstrap cycle cleanly
- Win32 **GDI** CDG view (`win32_gdi_view.c`, `desktop-gdi-rx.exe`, optional `--win-gdi` on `desktop-rx`) sharing the CPU RGBA raster with the GL path
- a separate TX CD+G source/memory slimdown spec now defines how to remove
  duplicated batch storage without changing protocol semantics in place
- desktop docs now center on **`docs/specs/desktop-platform-support.md`**
  (build matrix, GDI RX, sneakernet, retro bundle); older baseline / GUI
  feasibility notes live under **`docs/archive/`**

Still rough or incomplete:

- clock discipline is still software timestamped rather than venue-grade hardware-timestamped PTP
- long impaired-network soak data is still incomplete (including v4-focused runs)
- current FEC only repairs one missing payload per protected group
- some desktop proof scenarios can still show a small burst of early decode failures while queues settle after startup
- TX preview mode still falls back to a whole-memory `.cdg` load for the local
  OpenGL preview path even though default TX wire send is now random-access
  backed

## Important Current Limitations

- This is a desktop proof transport, not a finished venue-grade production stack.
- Long-duration impaired multicast soak data is still incomplete.
- Full asset replay is still required for deterministic seek/backfill even though snapshots now accelerate late join and recovery.
- TX no longer requires a full-memory `.cdg` preload for default wire send, but
  preview mode still uses a whole-memory fallback and later source-layer polish
  remains pending.
- Embedded receiver work is still documentation-first; there is no buildable ESP-IDF receiver in the repo yet.

## Related documentation

- `docs/README.md`: index of architecture, specs, tests, hardware, and **archive**
- `docs/specs/desktop-platform-support.md`: **master matrix** — Windows/Linux targets, `desktop-gdi-rx` / retro EXEs, Makefile flags, portable zips, sneakernet layout
- `docs/specs/win32-gdi-view-backend.md`: Win32 GDI view (`desktop-gdi-rx`, `desktop-gdi-tx`, `desktop-rx --gdi`, GL fallback)
- `docs/specs/windows-legacy-mingw-build.md`: MinGW PE/subsystem, XP/P3 profile, sneakernet script, DLL notes
- `docs/architecture/desktop-streaming.md`: end-to-end desktop TX/RX architecture and runtime diagrams (GL + GDI)
- `docs/architecture/threaded-streaming-runtime.md`: implemented task/queue ownership model for the threaded desktop runtime
- `docs/specs/transport-protocol.md`: full protocol v3 field-level documentation
- `docs/specs/tx-cdg-source-model.md`: current TX CD+G duplication and staged slimdown plan
- `docs/specs/receiver-progress-invariants.md`: receiver rules that prevent long-play ingress-without-playout stalls
- `docs/test/desktop-proof-plan.md`: what the desktop proof is intended to prove and how to read its status
- `docs/test/portability-streaming-validation.md`: portability/slimdown validation matrix for live wire behavior and platform smoke coverage
- `docs/test/win32-gdi-view-validation.md`: manual checklist for the GDI RX path
- `docs/test/desktop-impairment-validation.md`: repeatable impaired-network proof workflow and expected counters
- `docs/architecture/portable-core.md`: portable vs desktop-specific boundaries
- `docs/hardware/`: future ESP32 receiver and productization docs
- `docs/ops/quality-gates.md`: current tranche release criteria
- `docs/archive/`: superseded specs and research notes (kept for history)
