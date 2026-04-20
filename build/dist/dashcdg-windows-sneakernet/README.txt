dashcdg — Windows sneakernet bundle
====================================

Copy this entire folder (or use the sibling .zip). Each subfolder is self-contained
(EXE + DLLs next to them).

Subfolders
----------
windows-x64/
  64-bit MSYS2 mingw64. Headless desktop-tx + desktop-gdi-tx (Win32 preview, no GL);
  desktop-rx (GL default, GDI fallback on GL init failure) + desktop-gdi-rx + desktop-player.
  Audio SRC uses static libsoxr (LGPL) linked into EXEs — no libsoxr DLL.

windows-x86/
  32-bit mingw32. libopus-0.dll and libportaudio.dll come from build/mingw32-p3-vendor/
  (PIII / no-SSE2–safe; see scripts/build_mingw32_p3_opus_portaudio_shared.sh).
  Static libsoxr for SRC is built Pentium III–safe (no SSE SIMD engines); linked into EXEs.

windows-x86-legacy-p3/
  Same codecs as windows-x86; dashcdg objects use -march=pentium3 + XP PE (WINDOWS_LEGACY_TARGET=1).
  Includes desktop-legacy-rx.exe (copy of desktop-gdi-rx.exe) for muscle memory.

windows-x86-retro/
  Win2000-style PE, -march=pentium3, no OpenGL. desktop-retro-rx.exe / desktop-retro-tx.exe use real
  Opus decode/encode + PortAudio with the same PIII-safe libopus-0.dll and libportaudio.dll as other
  mingw32 folders (not WinMM-only). Default TX audio is Opus; use TTY `c` or flags to change codec.

Executables (standard folders)
------------------------------
  desktop-tx.exe         Headless transmitter (Opus or SBC-like via --badnet-v4 / profiles)
  desktop-gdi-tx.exe     Transmitter + Win32 GDI preview (no GL); --headless to hide window
  desktop-rx.exe         Receiver: OpenGL by default; --gdi forces GDI; Windows auto-fallback if GL fails
  desktop-gl-rx.exe      Same bits as desktop-rx.exe (alias)
  desktop-gdi-rx.exe     GDI-only receiver link (no GL DLL dependency for this EXE)
  desktop-legacy-rx.exe  Same bits as desktop-gdi-rx.exe (alias; x86 + legacy-p3 folders)
  desktop-player.exe     Local player + `tx` / `rx` subcommands (full GL + GDI code paths)
  desktop-*-player.exe   Copies of desktop-player.exe (legacy filenames)

