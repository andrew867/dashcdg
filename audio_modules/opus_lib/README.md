# Module: Opus (quality path)

**Implementation:** `platform/desktop/include/dashcdg/opus_codec.h`, `platform/desktop/src/opus_codec.c`

**Link:** system **libopus** (`-lopus`) on desktop builds; **stub** when `DASHCDG_DESKTOP_NO_OPUS=1` (retro).

**Wire:** v4 id **1**.

**No `vendor/`** — use distro / MSYS2 package or pinned binary deps per [`docs/specs/desktop-platform-support.md`](../../docs/specs/desktop-platform-support.md).
