# Desktop platform support matrix

## Purpose

Single contract for **what we build**, **where artifacts land**, and **how we
talk about OS support**. It supersedes ad-hoc baseline notes that used to live
in scattered markdown; **this file and `windows-legacy-mingw-build.md` are the
canonical Windows story.**

macOS is still out of scope for this tranche (no build hardware).

## Windows executables (current tree)

Built from `platform/desktop/src/` with shared core/proto libs.

| Binary | Role | Notes |
| --- | --- | --- |
| `desktop-tx.exe` | **Headless** transmitter (Windows build) | No OpenGL/FreeGLUT in this link: server-style TX only. **Protocol v4 by default** (`--v3` for legacy v3 loop). **Default audio:** resilience profile + **AMR-WB** (`--v4-audio-codec=amr-wb`); use `--audio-profile=quality` for Opus or `--v4-audio-codec=…` for other ids. **`--audio-profile=resilience`** does not override the codec. **`--help`** lists flags; TTY **`c`** cycles codecs. See [v4-audio-codecs.md](v4-audio-codecs.md). On Linux/macOS, `desktop-tx` is still the GL-capable object: default **no** window unless `--display` (use `desktop-player tx` for preview-on-by-default). |
| `desktop-gdi-tx.exe` | Transmitter + **Win32 GDI** preview (Windows) | No GL. Preview window **on** by default; `--headless` to hide. Same v4 codec defaults as `desktop-tx`. No local speaker monitor path (network send only, same as before). |
| `desktop-rx.exe` | Receiver, **OpenGL default** (Windows) | Tries GL first; **auto Win32 GDI** if `gl_renderer` init fails. `--gdi` or `--win-gdi` forces GDI. Decodes Opus, AMR-WB/NB, NB-IMA, low-rate QCELP, QCELP-13k, or SBC per **`v4_session_info`** / chunk `codec_id`; **`--help`** documents behaviour. |
| `desktop-gdi-rx.exe` | GDI-only receiver link | No GL imports. Opus + PortAudio. |
| `desktop-retro-tx.exe` | Retro transmitter | `WINDOWS_RETRO_BUNDLE=1`: GDI-era PE, no GL; links **Opus + PortAudio** (PIII-safe DLLs); default v4 codec **Opus** (change with `c` / flags). |
| `desktop-retro-rx.exe` | Retro GDI receiver | GDI + **PortAudio** output; **Opus** decode + other v4 codecs per session. |
| `desktop-player.exe` | Local player + `tx` / `rx` shims | Full GL + Win32 GDI code paths: `tx` preview on by default (`--headless` to disable); `rx` same GL→GDI fallback as `desktop-rx`. |

Implementation pointers:

- GDI window: `platform/desktop/src/win32_gdi_view.c`
- TX objects: `desktop_app_tx.o` (GL-capable / player), `desktop_app_tx_headless.o` (Windows `desktop-tx`), `desktop_app_tx_gdi.o` (`desktop-gdi-tx`), `desktop_app_tx_retro.o` (retro)
- GDI RX entry: `desktop_app_rx_gdi.o` from `app_rx.c`
- Retro RX/TX: `desktop_app_rx_retro_gdi.o`, `desktop_app_tx_retro.o`

## Windows portable zips (`make package` / `make dist-windows`)

Per-arch **debug** bundles (EXE + runtime DLLs copied into `build/<arch>/bin/` via `make bundle-runtime`):

| Artifact | Typical path |
| --- | --- |
| x64 portable zip | `build/amd64/release/dashcdg-windows-x64-portable.zip` |
| x86 portable zip | `build/x86/release/dashcdg-windows-x86-portable.zip` |

After `make dist-windows` or `scripts/build_release.sh all`, copies also appear under:

- `build/dist/dashcdg-windows-x64-portable.zip`
- `build/dist/dashcdg-windows-x86-portable.zip`

Standard zips on Windows MSYS2 include **`desktop-gdi-rx.exe`**, **`desktop-gdi-tx.exe`**, and headless **`desktop-tx.exe`** alongside GL-linked `desktop-rx` / `desktop-player` (see `Makefile` `debug` / `desktop-apps`).

## Sneakernet tree (`make dist-windows-sneakernet`)

`scripts/build_windows_sneakernet_dist.sh` builds **four** `make debug` variants and lays out:

- **Default (fast):** **no** `make clean` — incremental compile/link from existing `build/{amd64,x86,x86-retro}` trees. **`make -jN`** with `N` = `DASHCDG_SNEAKENET_JOBS` or CPU count. **Phase 1** builds **mingw64** and **mingw32** in **parallel** (disjoint `BUILD_DIR`); **phase 2** builds **legacy-p3** and **retro** in **parallel** (`build/x86` vs `build/x86-retro`).
- **Full rebuild:** set **`DASHCDG_SNEAKENET_CLEAN=1`** to run `make clean` before each variant (CI-style, slow).
- **Zip:** optional **`DASHCDG_SNEAKENET_ZIP_FAST=1`** uses `zip -1` when available instead of PowerShell `Compress-Archive`.

Layout:

- `build/dist/dashcdg-windows-sneakernet/windows-x64/`
- `build/dist/dashcdg-windows-sneakernet/windows-x86/`
- `build/dist/dashcdg-windows-sneakernet/windows-x86-legacy-p3/` (`WINDOWS_LEGACY_TARGET=1`, Pentium III–oriented **dashcdg** objects; **`libopus-0.dll`** / **`libportaudio.dll`** copied from **`build/mingw32-p3-vendor/`** like `windows-x86` — rebuild those DLLs for real PIII if you change Opus/PortAudio flags)
- `build/dist/dashcdg-windows-sneakernet/windows-x86-retro/` (`WINDOWS_RETRO_BUNDLE=1`): **`desktop-retro-rx.exe`** / **`desktop-retro-tx.exe`** plus minimal runtime DLLs; **no OpenGL / GLEW / FreeGLUT**; **Opus + PortAudio** use the same PIII-safe **`libopus-0.dll`** and **`libportaudio.dll`** copied from `build/mingw32-p3-vendor/` as in `windows-x86` (see `scripts/build_windows_sneakernet_dist.sh` and the tree `README.txt`).

Standard folders ship **`desktop-tx.exe`**, **`desktop-gdi-tx.exe`**, **`desktop-rx.exe`**, a **`desktop-gl-rx.exe`** alias (copy of `desktop-rx.exe`), **`desktop-gdi-rx.exe`**, and **`desktop-player.exe`** (+ legacy `desktop-*-player.exe` copies); see `README.txt` in that tree.

Each startup now prints a git-derived build identifier such as **`dev-master-g0b67b5a`** to stdout and to the sidecar soak log:

- `[tx] build: dev-master-g0b67b5a`
- `[rx] build: dev-master-g0b67b5a`

The sneakernet script pins one version string for the whole packaging run so every EXE in that bundle reports the same revision.

Optional zip: `build/dist/dashcdg-windows-sneakernet.zip` — PowerShell `Compress-Archive` by default, or **`zip -1`** when **`DASHCDG_SNEAKENET_ZIP_FAST=1`** and `zip` is on `PATH`.

## Makefile targets (Windows-focused)

| Target | Meaning |
| --- | --- |
| `make debug` | Core + proto + desktop lib + tests + `desktop-player`, `desktop-tx`, `desktop-rx`, **`desktop-gdi-rx.exe`** (Windows), retro pair when `WINDOWS_RETRO_BUNDLE=1`. |
| `make desktop-apps` | Same product set without `test-core`. |
| `make desktop-windows-x86-retro` | `clean debug` with `MINGW_ARCH=mingw32` `WINDOWS_RETRO_BUNDLE=1` (Win2000-style PE + `-march=pentium3` on dashcdg objects + retro binaries). |
| `make package` / `package-x64` / `package-x86` | Zip layout under `build/<arch>/release/`. |
| `make dist-windows` | `package-all-windows` + copy zips to `build/dist/`. |
| `make dist-windows-sneakernet` | Runs the sneakernet script above. |

Variables (see `Makefile`):

- `MINGW_ARCH=mingw64` | `mingw32`
- `WINDOWS_LEGACY_TARGET=1` — XP-oriented PE subsystem flags; on i686 adds `-march=pentium3 -mtune=pentium3` when retro bundle is off.
- `WINDOWS_RETRO_BUNDLE=1` — **requires** `mingw32`; switches `BUILD_DIR` to `build/x86-retro`, WinNT 5.0 defaults, same **`-march=pentium3`** object tuning as other pre-SSE2 profiles, `LDLIBS_DESKTOP_RETRO` (no OpenGL stack; **Opus + PortAudio** linked; runtime DLL copy list matches the retro Makefile rule — see `Makefile` `WINDOWS_RETRO_BUNDLE` / `bundle-runtime`).

**Windows TX/RX:** **`WINMM.dll`** for timer resolution; MMCSS uses **`AVRT.dll`** only when loadable (Vista+); XP/2000 skip AVRT (`win32_timing_boost.c`).

## Pixel path (GL vs GDI)

Both windowed RX paths rasterize with the same CPU contract:
[`cpu-rgba-raster-contract.md`](cpu-rgba-raster-contract.md) (`dashcdg_cdg_state_to_rgba8`).
GL uploads RGBA to a texture; GDI swaps to BGRA and blits via DIBSection.

## Linux

Linux remains a **documented source-build** target (OpenGL + GLEW + FreeGLUT +
PortAudio + Opus + pthread + IPv4 multicast/broadcast). Intended CPU families:
`amd64`, `x86`, `arm64`, `arm`. No release zip is produced in-tree today.

## OS support claims

Aligned with [`windows-legacy-mingw-build.md`](windows-legacy-mingw-build.md):

| OS | Build | Runtime / notes |
| --- | --- | --- |
| Windows 10/11 | Proven (host) | Primary baseline. |
| Windows 7 | Indirectly targeted by MinGW stack | Smoke not proven in tranche. |
| Windows Vista | Not proven | Research-only. |
| Windows XP (x86) | i686 packages + optional legacy link flags | GL driver stack still must be validated on real hardware; **GDI RX** reduces GL risk for receive-only scenarios. |
| Windows 2000 | Retro PE + `GetAdaptersAddresses` story | **Retro** bundle is the intentional minimal stack; still research-grade until hardware-soaked. |

Allowed messaging today:

- Windows **x64** and **x86** portable packaging exists and includes GL + GDI RX where built.
- **Sneakernet** layout documents four Windows variants for field USB copy.
- Linux multi-arch is an **intent**, not fully soaked.

Not allowed without new proof:

- “XP supported” / “2000 supported” as production claims
- Vista/7 runtime proven
- All Linux arches smoke-complete

## V4 TX audio flags (all desktop TX entrypoints)

Documented in detail in [`v4-audio-codecs.md`](v4-audio-codecs.md):

- **`--help` / `-h`** — full synopsis, defaults (**AMR-WB** on non-retro builds), and TTY hotkeys (**`c`** cycles codecs).
- `--v4-audio-codec=<name>` / `--v4-audio-codec <name>` — select v4 `audio_codec_id` (`opus`, `sbc-like`, `celp13k`, `qcelp8k`, `amr-nb`, `amr-wb`, `bluetooth-sbc`; legacy `evrc` alias still accepted for id 4).
- `--badnet-v4` — v4 + resilience + **amr-wb** (shorthand for the same default as a fresh TX).
- `--badnet-v4-sbc`, `--badnet-v4-qcelp8k` — resilience + wire id **2** or **4** (`--badnet-v4-evrc` remains a compatibility alias for id 4).
- `--audio-profile=resilience` — sets the resilience **profile** only; does **not** change the selected `audio_codec_id`.

## Further reading

- [`audio-codec-modules.md`](audio-codec-modules.md) — **GL/GDI-style** optional `audio_modules/*` backends + vendoring script for every linked codec repo
- [`v4-audio-codecs.md`](v4-audio-codecs.md) — wire IDs, fixed-point narrowband, MCU portability
- [`v4-display-audio-sync.md`](v4-display-audio-sync.md) — TX preview delay vs RX playout (normative for future work)
- [`v4-network-stats-and-adaptation.md`](v4-network-stats-and-adaptation.md) — stats / adaptation design (wire TBD)
- [`vendored-opus-portaudio-windows.md`](vendored-opus-portaudio-windows.md) — vendored libopus / PortAudio builds (retro / MCU roadmap)
- [`../test/v4-network-observability-validation.md`](../test/v4-network-observability-validation.md) — future QA checklist
- [`windows-legacy-mingw-build.md`](windows-legacy-mingw-build.md) — PE audit, DLL lists, retro profile
- [`win32-gdi-view-backend.md`](win32-gdi-view-backend.md) — GDI backend behavior
- [`../architecture/desktop-streaming.md`](../architecture/desktop-streaming.md) — end-to-end TX/RX + render paths
