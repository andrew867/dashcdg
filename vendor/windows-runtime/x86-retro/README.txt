Optional override DLLs for sneakernet packaging (copied first by `make bundle-runtime`):
  libopus-0.dll   — P3 rebuild (see scripts/rebuild_mingw32_opus_pentium3.sh)
  libportaudio.dll — P3/XP-safe rebuild (see scripts/rebuild_mingw32_portaudio_pentium3.sh)
  glew32.dll, libfreeglut.dll — only if testing GL on legacy builds; GDI builds omit these.
