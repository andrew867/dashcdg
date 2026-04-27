# Narrowband and low-bitrate audio quality — end-to-end paths, root causes, tests, and fix plan

## Document control

| Field | Value |
| --- | --- |
| **Status** | **Implemented on desktop (2026)** — FIR decimation + Lanczos upsample paths, overlap SRC **`skip_out`** clamp, TX **~80 Hz HPF** + **~3 dB headroom** before speech-codec encode, **Opus bypass** of that speech headroom path, soft limiting on hot PCM, codec-handoff startup skip-hold tuning, **idle / unpause** **`playback_base_*`** hygiene; jitter **PLC** and **TX mic linear resample** anti-alias remain optional follow-ups. |
| **Scope** | Desktop TX/RX for **v4 narrowband family** (wire ids 2–7) and, where relevant, **Opus at very low bitrates**; “choppy / not continuous / shrill or harsh” reports. |
| **Related** | [v4-audio-codecs.md](v4-audio-codecs.md), [bad-network-audio-profiles.md](bad-network-audio-profiles.md), [audio-jitter-playout-boundary.md](audio-jitter-playout-boundary.md) |

## 1. Symptom vocabulary (for alignment)

| User phrase | Likely technical reading |
| --- | --- |
| **Chopped, not a continuous low-quality stream** | Time gaps (missing frames, underrun, or PLC absent), or **gross amplitude / spectral discontinuities** at frame or packet boundaries (independent block processing, packet-loss concealment gaps). |
| **Shrill, harsh** | **Aliasing** and/or imaging on bad rate changes, codec artifacts at low rate, or **mismatch of Opus “music” mode** to speech; less often **Cubic/ polynomial ringing** on sharp transients. |
| “Low bitrate” in this product | (a) **Narrowband vocoders** (4.75–24 kbps class) + **NB-IMA**; (b) **Opus** when configured or forced to a very low `bitrate_bps` (see `platform/desktop/src/opus_codec.c`). |

## 2. End-to-end code path inventory (desktop)

### 2.1 Transmitter (encode)

| Stage | Code | Notes |
| --- | --- | --- |
| **Capture / file decode to PCM** | `app_tx.c` (audio thread), `desktop_audio.c` | Chunks of PCM at device or MP3 native rate, assembled into 20 ms @ 48 kHz stereo where applicable. |
| **Resample to 48 kHz** (when input rate ≠ 48 k) | `dashcdg_tx_resample_pcm` in `app_tx.c` | **Linear** interpolation; **no explicit anti-aliasing** before any implicit decimation. |
| **Narrowband codec** | `nb_ima_codec.c` (id 2), `amr_nb_codec.c` / `amr_wb_codec.c`, `nb_evrc_codec.c`, `nb_qcelp_codec.c`, `nb_sbc_codec.c` | All except **NB-IMA** use `dashcdg_pcm_mono_resample_cubic` to move between 48 kHz and **8 kHz / 16 kHz** as required (see table below). |
| **Opus (quality path)** | `opus_codec.c` + `app_tx.c` | Encoder uses `OPUS_APPLICATION_AUDIO`, `OPUS_SIGNAL_MUSIC`, VBR, complexity 5 — for **very low** Opus bitrates, **VOICE/ AUTO bandwidth** is often a better default than MUSIC (out of scope until Opus is part of the reported failure). |

**Per-codec 48 kHz interface (20 ms = 960 mono samples @ 48 k):**

| Codec (wire id) | Internal timeline | Resampler in adapter |
| --- | --- | --- |
| **NB-IMA** (2) | 8 kHz-equivalent, 160 “narrowband” samples / frame | **Custom** 6:1 int down/up in `core/src/nb_ima_codec.c` (not `pcm_rate_convert.c`). |
| **QCELP** (3) | 8 kHz | `dashcdg_pcm_mono_resample_cubic` 48↔8 k, `nb_qcelp_codec.c` |
| **EVRC** (4) | 8 kHz | `dashcdg_pcm_mono_resample_cubic` 48↔8 k, `nb_evrc_codec.c` |
| **AMR-NB** (5) | 8 kHz | `dashcdg_pcm_mono_resample_cubic` 48↔8 k, `amr_nb_codec.c` |
| **AMR-WB** (6) | 16 kHz | `dashcdg_pcm_mono_resample_cubic` 48↔16 k, `amr_wb_codec.c` |
| **Bluetooth SBC** (7) | 16 kHz mono path inside adapter | `dashcdg_pcm_mono_resample_cubic` 48↔16 k, `nb_sbc_codec.c` |

### 2.2 Receiver (decode + playout)

| Stage | Code | Notes |
| --- | --- | --- |
| **Jitter / reorder** | `core/src/audio_jitter.c`, `app_rx.c` | `dashcdg_audio_jitter_drain_step`: if the next `media_sequence` is missing, may **SKIP** (advance, count misses) with **no synthesized PCM** — perceived as a **time hole** (“chop”). |
| **Decode to 48 kHz** | `app_rx.c` `dashcdg_rx_apply_audio_frame_locked` | Branches to same adapters as TX; same resamplers. |
| **Mono → interleaved stereo** | `dashcdg_rx_pcm48_mono_to_interleaved_stereo` | Copy mono to L/R. |
| **Device queue** | `dashcdg_desktop_audio_queue_frames` | Back-pressure: partial accept documented in `app_rx.c` (slot advances); can reduce smoothness if chronic. |
| **PCM ring sizing (NB)** | `dashcdg_rx_network_stream_ring_ms` in `app_rx.c` | **≥ 2000 ms** for narrowband codecs to absorb jitter; reduces underrun risk but does **not** add concealment for **loss**. |

**Critical observation:** the **resampler is invoked per 20 ms frame** with a full buffer of 8 k/16 k samples. It is **not a streaming** resampler with cross-frame state (taps, phase history) beyond what the **speech decoder** already keeps internally.

## 3. Why this can sound “harsh / shrill” (hypotheses, ordered)

1. **No anti-aliasing filter** on 48 kHz → 8/16 kHz: `dashcdg_pcm_mono_resample_cubic` (`pcm_rate_convert.c`) is a **cubic (Catmull–Rom) interpolator in time** — it is **not** a band-limited resampler. Energy **above 4 kHz (or 8 kHz for 16 k paths)** is **not** removed before the logical “rate change”; it **folds/aliases** into the band the vocoder will then encode, then gets played back at 48 kHz after the inverse step — often heard as **grain, hiss, or “metallic / harsh”** timbre, especially on content with HF energy (sibilance, room noise, or music).  
2. **Same for TX live path** when the mic is not 48 k: `dashcdg_tx_resample_pcm` is **linear**; again **no LPF** before resampling.  
3. **NB-IMA** uses a **fixed-point, frame-local** 6:1 down (weighted sum) and 6:1 up (linear blend). That is **not the same** frequency-domain behavior as the cubic path used in other NB codecs; it may be **inconsistent in artifacts** and still allows **imaging** relative to a properly band-limited 8 kHz representation.  
4. **Opus (if in scope)**: `OPUS_SIGNAL_MUSIC` + `OPUS_APPLICATION_AUDIO` can behave poorly at **very low** bitrates for **speech** (different bandpass / stereo artifacts not applicable to mono, but “wrong tool” for speech intelligibility and perceived harshness). **NB vocoders** are a separate issue.  
5. **SBC** (`nb_sbc_codec.c`) uses 16 kHz and **low bitpool**-class settings — already aggressive; resampler aliasing **stacks** with SBC’s own band limiting.

## 4. Why it can sound “chopped / non-continuous” (hypotheses, ordered)

1. **Jitter `DRAIN_SKIP`** when a sequence number is **missing** and the late policy triggers: the clock advances **without** producing 20 ms of PCM for that step — a **true gap** unless PLC fills it (`core/src/audio_jitter.c`, `app_rx.c`).  
2. **Decode failure** on bad packets (`decoded_frames <= 0`) increments failures and can drop a frame.  
3. **Device / queue** partial writes or underflow in `desktop_audio` (less likely for NB with a 2 s min ring, but still possible on stressed hosts or **null** / **mute** edge cases).  
4. **Perception**: strong **ADPCM / vocoder** frame **energy** pumping at low bitrates is often described as “choppy” even when **time-continuous** — A/B with lossless 48 k → band-limited 3.4/7 k will separate **codec brutality** from **transport** gaps.  
5. **NB-IMA** and others: **independent resample per frame** can create **very small** boundary discontinuities that sound like “ticks” in edge cases, though **decoder state** in EVRC/AMR usually reduces worst-case steps.

## 5. Proposed quality bar (acceptance)

| Metric | Target (initial) | Method |
| --- | --- | --- |
| **Alias / imaging** (48→8) | A-weighted or plain RMS **noise floor** in 4 kHz+ band on a 997 Hz (or 1 kHz) input **significantly lower** than current cubic path; document ratio in dB. | Fixed test harness, FFT or bandpass. |
| **Block boundary** | No sample jump larger than a defined threshold (e.g. < X % of full scale) between the last 48 k sample of frame *n* and first of *n+1* on **synthetic continuous** 8 k sine **through** the codec chain, **excluding** expected vocoder model error. | Golden vector / `memcmp` of envelope where applicable. |
| **Loss behavior** | With **1 % random frame drop**, subjective “chop” not worse than **repeat-last-frame** PLC (or **silence** if we explicitly choose silence); measure pop energy. | Simulated `DRAIN_SKIP` or injected loss. |
| **Regression** | `test_nb_ima_codec_roundtrip` in `tests/test_core.c` remains green; new tests added for new resampler. | CI |

## 6. Test plan (phased, automatable)

### 6.1 Resampler in isolation (no network)

- **Sine / multi-tone** sweeps: 200 Hz – 3.5 kHz @ 48 k, decimate to 8 k and back to 48 k, measure **energy** above 4.5 kHz (should be near noise floor for a good design).  
- **White or pink** noise: same, measure **spectral flatness** of “image” products.  
- **Impulse** or gated sine: check **Cubic** vs **sinc/FIR** ringing and peak overshoot.  
- **Contiguity**: concatenate **100+** 20 ms blocks: track **max |Δ| between last sample of block and first of next** on a signal that is **continuous in source rate** (feeds 8 k representation without codec).

**Implementation note:** new tests can live in `tests/test_core.c` (or a dedicated `tests/test_resampler.c`) and link `pcm_rate_convert.c` and any replacement.

### 6.2 Per-codec roundtrip (file or synthetic PCM)

- **Roundtrip** TX adapter → mock frame → RX adapter in-process for: AMR-NB, AMR-WB, EVRC, QCELP, NB-IMA, SBC (as practical).  
- **Compare** to **reference** chain: 48 k → (high-quality offline) → 8/16 k → (same) → 48 k, to establish an **upper bound** on what “good” looks like.  
- Optional **MOS/visqol** or simple **PESQ**-style if tooling is available; otherwise **STOI/SNR** proxies are enough for CI.

### 6.3 Live network / jitter (integration)

- Two processes or loopback: v4, narrowband id under test, **injected** `DASHCDG_AUDIO_DRAIN_SKIP` (or real drops) to verify **PLC** once implemented.  
- Log `audio_decode_failures`, `audio_queue_overflows`, `audio_missing_skips` in receiver (`app_rx.c` / stats) under bad-network profile.

## 7. Fix / implementation plan (phased, for a follow-up tranche)

### Phase A — **Measurement and guardrails (non-breaking)**

- Add **opt-in** logging or counters: resampler type, per-frame max gradient at NB boundary, `audio_jitter` skip count vs playout.  
- Capture **short** PCM dumps (TX pre-codec, RX post-decode) behind a build flag for lab triage.  
- Document **where** the narrowband 2 s ring is chosen (`dashcdg_rx_network_stream_ring_ms`) and confirm it is **not** compensating for **consecutive** `SKIP` holes.

### Phase B — **Band-limited resampling (desktop, shared)**

- Implemented first tranche: exact-ratio **FIR low-pass decimation** for **48 -> 8 kHz** and **48 -> 16 kHz** inside `pcm_rate_convert.c`, keeping the existing API used by AMR / EVRC / QCELP / SBC wrappers. This removes the worst alias source before narrowband encode without forcing a desktop dependency bump.  
- Added a regression test binary that asserts:
  - DC survives exact-ratio decimation unchanged.
  - A deliberately alias-prone alternating 48 kHz pattern is strongly rejected in the interior of the 8/16 kHz outputs.
- Remaining follow-up:
  - replace the current **8/16 -> 48 kHz** cubic expansion with a similarly band-limited interpolation path,
  - decide whether desktop should later move to **libsamplerate / Speex** or keep the in-tree exact-ratio FIR implementation,
  - evaluate whether **NB-IMA** should share the same front-end decimator.
- **Unify** NB-IMA: either (i) pre-filter + keep current int 6:1, or (ii) feed **8 k** from a shared 48→8 **band-limited** path into NB-IMA **instead of** the in-file weighted downsample, if wire format and bit-exact interop with older peers allow (may require a **bump** or a **“codec profile”** flag; **spec decision**).  

### Phase C — **Jitter: concealment, not just delay**

- On `DRAIN_SKIP` or decode failure, insert **20 ms of PLC**: **repeat last** decoded frame, **attenuate** (fade) after repeated loss, or **light noise fill**; avoid **raw silence** unless the stream is “voice inactive”.  
- Align with **resilience** profile in [bad-network-audio-profiles.md](bad-network-audio-profiles.md) (redundancy, shorter dependency chains) — this doc addresses **acoustic** continuity, not just transport.  

### Phase D — **Opus low-bitrate (only if in scope of reports)**

- Add profile paths: e.g. `OPUS_SET_SIGNAL(VOICE)` for speech, **or** `OPUS_AUTO`, and consider **constrained** wideband; gate by session **content type** or a CLI flag.  
- Keep **MUSIC** for actual music sessions.

### Phase E — **Documentation and defaults**

- Update [v4-audio-codecs.md](v4-audio-codecs.md) with: **resampler** identity, **embedded** float vs fixed, and any **version** of NB-IMA downsample.  
- Add a **troubleshooting** line: *“Harsh: likely alias from `pcm_rate_convert` cubic; chop: likely jitter SKIP without PLC.”*

## 8. Risks and open decisions (for your review)

| ID | Question |
| --- | --- |
| R1 | **Bit-exact NB-IMA** vs all peers: can we change the 48→8 **front-end** without a protocol bump? (May need a **“compatible”** vs **“v2”** IMA pre-filter.) |
| R2 | **MCU**: must 8 k resampler be **int-only** on day one, or is **soft-float** acceptable for desktop-only first? |
| R3 | **SBC** bitpool vs quality: is raising bitpool in scope, or resampler-only? |
| R4 | Are reports **100 % narrowband** or is **low-bitrate Opus** in the same bucket? (Influences Phase D priority.) |

## 9. References (code)

- Cubic resampler, no LPF: `platform/desktop/include/dashcdg/pcm_rate_convert.h`, `platform/desktop/src/pcm_rate_convert.c`  
- TX linear resampler: `platform/desktop/src/app_tx.c` — `dashcdg_tx_resample_pcm`  
- NB-IMA: `core/src/nb_ima_codec.c`  
- AMR / EVRC / QCELP / SBC adapters: `platform/desktop/src/amr_*.c`, `nb_evrc_codec.c`, `nb_qcelp_codec.c`, `nb_sbc_codec.c`  
- Opus encode tuning: `platform/desktop/src/opus_codec.c`  
- RX apply + jitter: `platform/desktop/src/app_rx.c`, `core/src/audio_jitter.c`  
- NB playout buffer floor: `dashcdg_rx_network_stream_ring_ms` in `app_rx.c`  

## 10. Desktop updates (2026, summarized)

Operational fixes landed in **`platform/desktop`** and **`pcm_rate_convert.c`**:

- **Alias / peaks:** Exact-ratio **FIR decimation** for **48 → 8 / 16 kHz** before vocoder encode; Lanczos-style upsampling with **soft saturation** toward **int16** on hot peaks; mono/stereo **overlap SRC** alignment clamp to avoid whole-frame silence at chunk boundaries.
- **Levels:** **Q15 ~−3 dB** encode headroom (**`DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15`**) on speech-codec TX paths; **Opus** now bypasses that pad. **80 Hz HPF** remains TX-before-narrowband only; RX no longer stacks a second HPF on NB (avoid cascaded tilt).
- **Transport feel:** **Warm** startup skip-hold when codec swaps mid-stream; **cold** idle→TX and **resume** clear **`playback_base_*`** so **`claim_audio_start`** and jitter see a consistent timeline.
- **Unpause:** **`dashcdg_rx_rearm_live_video_after_unpause_locked`** restores **CDG bridge** / skip-hold toward **`live_state`**; **`dashcdg_rx_reprime_audio_after_host_underrun_locked`** clears audio jitter and stops the host stream so playout can re-prime cleanly.

Related: **`docs/specs/opus-desktop-encoding-policy.md`**, **`AGENTS.md`**.

---

**Status for implementation:** Phase **B** (band-limited resampling core) is largely satisfied on desktop; **Phase C** (PLC on **`DRAIN_SKIP`**) remains open if loss-soak still reports audible holes.
