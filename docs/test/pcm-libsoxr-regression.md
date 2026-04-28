# Regression plan — PCM libsoxr migration (historical)

This plan is retained for historical migration evidence. For current desktop runtime validation, prioritize:

- `make test` with `test-pcm-rate-convert`,
- desktop RX/TX sync and startup regression scenarios from active v4 validation docs,
- mixed GL/GDI runtime soak checks.

## Automated (`make test`)

Runs `tests/test_pcm_rate_convert` linked with `desktop_pcm_rate_convert.o` and **libsoxr when `DASHCDG_HAVE_LIBSOXR`** (Windows MinGW / Linux with vendored `build/soxr-vendor/install`).

| Test | Intent |
| --- | --- |
| `test_dc_is_preserved_on_exact_narrowband_decimation` | DC stability 48→8/16 kHz mono. |
| `test_alias_prone_high_frequency_is_rejected` | HF folding suppressed (bounds may be tuned after SoXR). |
| `test_dc_preserved_on_8k_to_48k_upsample` | DC continuity 8→48 kHz (steady-state band excludes edges). |
| `test_sinc_resample_441_to_480_yields_sine_energy` | Non-trivial energy after 44.1→48 kHz sine. |
| `test_sinc_resample_preserves_linearity_on_hot_programme` | Superposition approximate (small max diff). |
| `test_overlap_chunk0_matches_isolated_resample` | Overlap vs non-overlap identical on chunk 0. |
| `test_overlap_stereo_48k_to_441_chunks_match_long_buffer` | Chunked overlap tracks full-buffer reference (allowed numeric slack after SoXR). |
| Stereo/mono averaging tests | Unchanged math (not SRC). |

After migration, **re-run** `make test` on:

- `MINGW_ARCH=mingw64` (optional `mingw32`).
- Linux with vendored soxr (`scripts/build_soxr_vendor.sh` native prefix) if available.

## Manual (operator)

1. **TX codec cycle (`c`)** while playing MP3+G: audio must **not** stick silent or spam errors; HUD codec name matches audio; receivers recover (see `v4-codec-switching-contract.md`).
2. **NB codecs** (AMR-WB, AMR-NB, EVRC, CELP, SBC): subjective absence of harsh **alias chirp** on dense programme vs Opus baseline on same clip.
3. **Sneakernet** zip: `bash scripts/build_windows_sneakernet_dist.sh` completes; spot-check `desktop-tx.exe` / `desktop-rx.exe` from `windows-x64`.

## Failure triage

- **Link errors for test binary**: ensure `TEST_PCM_RATE_CONVERT_BIN` recipe appends `$(SOXR_LINK)` when non-empty.
- **Overlap chunked vs full-buffer**: `tests/test_pcm_rate_convert.c` allows `max_diff <= 3000` sample units on the long stereo 48→44.1 chunk comparison (SoXR chunked mono passes diverge more than legacy Lanczos from one-shot stereo reference).

## Locked numeric gates (mingw64, libsoxr 0.1.3)

Values are empirical guards after migration — revisit if SRC quality settings change:

- Narrowband DC mid-buffer (48→8/16 kHz): samples within **9700–10300** for flat 10000 input (passband ripple).
- 8 kHz → 48 kHz DC band (mid-window): **7560–7990** for flat 7777 input.
- Superposition max sample delta (441→48 Programme): **≤ 48**.
- Overlap chunk0 vs isolated resample: **≤ 4** sample delta.
- Five-chunk overlap vs reference: **≤ 3000** sample delta.
