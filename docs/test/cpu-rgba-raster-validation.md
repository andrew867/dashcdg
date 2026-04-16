# CPU RGBA raster — validation matrix

## Unit tests (`make test`)

| ID | Scenario | Pass criteria |
| --- | --- | --- |
| RZ-01 | `dashcdg_cdg_state_init` then raster | All alpha `255` (or 254 per formula) for default palette; deterministic byte vector. |
| RZ-02 | Transparency index | Set `transparency[i]=63` for used index → alpha `0`. |
| RZ-03 | Tile offset clamp | Set `display_h_offset` past clamp; sampling matches spec (same pixel as GL golden). |
| RZ-04 | Known color table | `color_table[0]=0x00FF0000` at index 0 fill → output RGB `(255,0,0)`. |

Golden vectors are **embedded as hex byte arrays** in `tests/test_core.c` (no external fixture files required for CI portability).

## Cross-check (optional manual)

| ID | Steps | Expected |
| --- | --- | --- |
| M-RZ-01 | Same `cdg_state` rendered on desktop GL and dumped via future debug hook | Byte diff within tolerance if any FP path remains (target: **zero diff** with integer alpha). |

## Performance (informative)

Raster cost for `288×192×4` is trivial on desktop; on ESP32 profile, measure separately under `docs/specs/embedded-rx-audio-profile.md` thermal budget if combined with Wi-Fi.
