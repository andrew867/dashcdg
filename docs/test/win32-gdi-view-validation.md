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

## Soak

- 10+ minutes windowed on Windows 10 x64 with live stream (stretch goal: XP x86 VM per retro plan).
