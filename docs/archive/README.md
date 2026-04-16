# Documentation archive

Material moved here is **kept for history and research context**. It is **not**
the canonical description of the current product unless a main doc explicitly
points here.

| Path | Former location | Why archived |
| --- | --- | --- |
| `architecture/legacy-windows-gui-feasibility.md` | `docs/architecture/` | Pre‑GDI research: “full OpenGL GUI on ancient Windows” feasibility; real shipping path is now **Win32 GDI** (`desktop-gdi-rx.exe`, `--win-gdi`) plus **retro** bundle. |
| `architecture/modern-desktop-baseline.md` | `docs/architecture/` | Tier list superseded by **`docs/specs/desktop-platform-support.md`** (single matrix for builds, OS claims, and artifacts). |
| `specs/windows-retro-graphics-backend.md` | `docs/specs/` | Forward‑looking sketch (DDraw, text mode, etc.). **Implemented today:** CPU RGBA → `win32_gdi_view.c` + optional **`desktop-retro-*`** (no GL/Opus). DirectDraw / D3D7 tracks remain future work if needed. |
| `test/windows-retro-graphics-validation.md` | `docs/test/` | Companion to the archived retro‑graphics **spec**; not the validation plan for the shipped GDI path (see **`docs/test/win32-gdi-view-validation.md`**). |

For current Windows desktop behavior, start with:

- `docs/README.md` (index)
- `docs/specs/desktop-platform-support.md`
- `docs/specs/win32-gdi-view-backend.md`
- `docs/specs/windows-legacy-mingw-build.md`
