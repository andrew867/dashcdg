# Win32 GDI view — validation matrix

## Automated (host)

| ID | Check |
| --- | --- |
| GDI-B-01 | `make debug` on MSYS2/MinGW64 links `desktop-rx` with `-lgdi32 -luser32` |
| GDI-B-02 | Non-Windows `make debug`: `--win-gdi` rejected at runtime if passed (or compile-time unavailable) |

## Manual (Windows)

| ID | Steps | Pass criteria |
| --- | --- | --- |
| GDI-M-01 | Launch `desktop-rx <addr> <port> --win-gdi` against a TX | Window appears, CDG animates, audio plays |
| GDI-M-02 | Resize window | No crash; image scales; 1×1 client does not divide-by-zero |
| GDI-M-03 | Keys I / M / S | HUD toggles, mute toggles, status line to stdout |
| GDI-M-04 | Close window | Process exits cleanly (no hang in media thread) |
| GDI-M-05 | Click-drag move while active audio/video is running | No crash; window remains responsive; playback continues |
| GDI-M-06 | Repeated minimize/restore during playback | No crash, no permanent black frame, decode continues |
| GDI-M-07 | Rapid open/close cycles (5+ launches) | No stuck process; each close exits cleanly |

## Soak

- 10+ minutes windowed on Windows 10 x64 with live stream (stretch goal: XP x86 VM per retro plan).
