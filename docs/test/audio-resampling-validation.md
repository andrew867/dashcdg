# Audio resampling validation (automated)

## Purpose

Regression-proof the desktop PCM rate engine ([`pcm_rate_convert.c`](../../platform/desktop/src/pcm_rate_convert.c)): **exact-ratio FIR**, **integer upsample**, and **sinc arbitrary-ratio** paths must not silently degrade.

## Automated tests

| Binary | Source | Invoked by |
|--------|--------|------------|
| `build/bin/test-pcm-rate-convert` | [`tests/test_pcm_rate_convert.c`](../../tests/test_pcm_rate_convert.c) | `make test` |

### Cases (minimum)

1. **DC preservation** — Constant input across 48→8 / 48→16 decimation yields stable DC within ±1 LSB (existing).
2. **HF alias rejection** — Alternating ±full-scale at 48 kHz collapsed to 8 k / 16 k must yield low residual energy (existing heuristic bounds).
3. **Upsample 8→48 / 16→48** — DC preservation and bounded HF energy after interpolation LP.
4. **Arbitrary-ratio sinc** — e.g. 441→480 ratio short vector: finite output, no NaNs, bounded peak after sine input.

## Golden vectors (optional regeneration)

Goldens may be regenerated using an **approved reference** (SoX CLI, Mathematica, or scipy) off-tree; checked-in tests **derive thresholds programmatically** where possible to avoid large binary blobs.

When updating FIR taps or sinc width, rerun `make test` and bump documented tolerance only with reviewer sign-off.

## Manual listening (non-blocking)

Short A/B clips through TX capture resampler remain optional subjective QA; **CI gate is numeric only**.

## References

- [`docs/specs/audio-rate-conversion-engine.md`](../specs/audio-rate-conversion-engine.md)
