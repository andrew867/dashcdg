# ESP32 v4 audio decode + sync (badge / karaoke) — difficulty and plan

**See also:** [`docs/embedded/esp32-karaoke-decode-toggles-video-regression-rca.md`](../embedded/esp32-karaoke-decode-toggles-video-regression-rca.md) — what decode toggles actually gate, CDG vs audio resource contention, and slot-count tradeoffs.

**Refactor package:** [`docs/specs/esp32-heap-backed-jitter-rings-spec.md`](esp32-heap-backed-jitter-rings-spec.md), [`docs/specs/esp32-heap-backed-jitter-rings-test-plan.md`](esp32-heap-backed-jitter-rings-test-plan.md), and [`docs/embedded/esp32-heap-backed-jitter-rings-implementation-plan.md`](../embedded/esp32-heap-backed-jitter-rings-implementation-plan.md).

## TL;DR difficulty

| Area | Effort | Why |
|------|--------|-----|
| Wire + session + clock | **Low–medium** | `badge_rx.c` already joins multicast, parses v4, runs `handle_clock_sync`, owns `dashcdg_media_clock_t`. **Audio datagrams are not handled yet** — add `DASHCDG_PACKET_V4_AUDIO_CHUNK` to the same `rx_one_datagram` switch and feed a new pipeline. |
| **Sync / timing discipline** | **Medium** | Desktop proves the contract in `platform/desktop/src/app_rx.c` (jitter priming, `playback_base_*`, cold join, pause). **ESP32 `dashcdg_core` today does *not* compile `core/src/audio_jitter.c`**, so there is **no shared “drop in” audio playout module** on the badge yet — either add `audio_jitter.c` (+ glue) to `components/dashcdg_core` or grow a badge-local ring + scheduler that mirrors the same invariants (documented in `docs/specs/v4-codec-switching-contract.md` and `AGENTS.md`). |
| **Decoders (your order)** | **Medium → high** | **AMR-WB (6)** and **AMR-NB (5)** have a known desktop path (`amr_*_codec.c` + vendor tree per `audio-codec-modules.md`). **QCELP-13k (3)** and **Bluetooth SBC (7)** are desktop-wrapped vendor C; **Opus (1)** is `libopus` + `opus_codec.c`. None of that is linked on ESP32 today — each is a **port + CMake + heap budget + stack** exercise. |
| **Audio lab** | **Low once karaoke path exists** | Audio lab today is **PWM lab PCM** (`badge_lab_ym.c`), not v4. Reuse is “same output stage + volume” if you add a “stream lab sample” mode; **not** the same code path as multicast RX until you factor a small “PCM → amp” API. |

**Bottom line:** not “really fucking hard” as cryptography, but **not a weekend** either: it is a **multi-milestone** port with **real soak** after each codec and after Wi‑Fi + CDG + audio together.

## What is already centralized vs forked

**Reusable on ESP32 with little friction**

- **Framing / parse:** `dashcdg_protocol_*` (`dashcdg_proto` component) — already on the badge.
- **Clock API:** `dashcdg_media_clock_*` (`core/src/media_clock.c` in `dashcdg_core`) — already compiled.
- **v4 clock_sync + session_info fields** — badge already has handlers; extend with audio codec id / sample rate from `session_info` when you add audio.

**Reused conceptually, not copy-paste from one binary**

- **Desktop `app_rx.c`** is the **reference implementation** for jitter, priming, codec hot-swap, and A/V alignment. There is **no single shared “v4 audio engine” .c file** today that both desktop and ESP32 link; expect to **extract or parallel** the smallest subset (ingest timestamps, reorder, playout clock) once behaviour is frozen.

**Not on the badge build today**

- `core/src/audio_jitter.c`
- All of `platform/desktop/src/*codec*.c` and `opus_codec.c`

## Codec priority (your order) vs repo baseline

Your order: **AMR-WB → QCELP-13k → SBC → Opus (last)**.

**Badge bring-up policy (2026):** skip **NB-IMA (wire id 2)** as a development milestone. Prefer **AMR-WB (wire id 6)** first on ESP32 (same `codec-amr` vendor + desktop `amr_wb_codec.c` / `pcm_rate_convert.c` stack) because it matches the desired on-air speech quality and the desktop TX/RX path already in use. NB-IMA remains available in `core/` for interoperability if explicitly needed later.

Repo embedded policy (`docs/specs/embedded-rx-audio-profile.md`) may still mention NB-IMA-first language; treat this roadmap as the **badge** source of truth until that doc is reconciled.

1. **Phase 0 (prove audio path):** v4 audio chunk ingest + **`core/src/audio_jitter.c`** + playout clock from **`media_clock` / `V4_CLOCK_SYNC`** + **AMR-WB decode** to the board audio stage (DAC or I2S), with desktop TX on `--v4-audio-codec=amr-wb`.
2. **Phase 1:** Harden **AMR-WB** — session/codec switch without leak, soak with CDG + Wi-Fi, measure heap/WDT.
3. **Phase 2:** **QCELP-13k (3)**.
4. **Phase 3:** **Bluetooth SBC (7)** — confirm **byte-for-byte** framing matches desktop `nb_sbc_codec` output (kernel `sbc` LGPL — static link policy in `audio-codec-modules.md`).
5. **Phase 4:** **Opus (1)** — `libopus` on ESP32 is doable (Espressif examples, PSRAM helps); treat as **optional** build flag until CPU/RAM headroom is measured **with CDG blit load**.

## Implementation phases (NASA-style gates)

### Phase 0 — Audio existence (gate: AMR-WB audible on target)

- [ ] Add `DASHCDG_PACKET_V4_AUDIO_CHUNK` handling in `badge_rx.c` (or dedicated task fed from RX).
- [x] Add `audio_jitter.c` to `components/dashcdg_core` (same module as desktop).
- [ ] Board audio output (native DAC continuous @ 48 kHz mono **or** I2S); playout clock from `media_clock` + `V4_CLOCK_SYNC` (same fields desktop uses).
- [ ] **Tests:** cold join, late join, 60 s idle RX then TX start, no wedge; log underruns/overruns.

### Phase 1 — AMR-WB (gate: stable lyric alignment “good enough”)

- [ ] Port or wrap **same vendor** as desktop (`audio_modules/amr_pschatzmann` per `audio-codec-modules.md`).
- [ ] `session_info` reconfigure: AMR-WB ↔ other without leak; match desktop `need_audio_device_reconfigure` semantics.
- [ ] **Soak:** 30+ min with CDG + Wi‑Fi + volume; measure worst-case heap + task WDT.

### Phase 2 — QCELP-13k

- [ ] Vendor `qcelp_rupw` build for ESP-IDF; same jitter ingest API as Phase 1.

### Phase 3 — SBC

- [ ] Kernel SBC or equivalent; verify interop with desktop TX for id **7**.

### Phase 4 — Opus

- [ ] `libopus` + decoder config; optional `CONFIG_*` to disable on small flash builds.

## “Ready for soak test” definition

Call the embedded path **soak-ready** when:

1. **TX = desktop-tx** (or lab sender) on **each enabled codec**, RX = badge, **≥30 min** each with no reboot, no WDT, no heap death spiral.
2. **Cold join / pause / resume** match desktop checklist in `AGENTS.md` for that codec.
3. **CDG + audio** together: no systematic late video vs audio beyond agreed threshold (document measured ms).
4. **Audio lab** (if wired): only exercises **approved** sample paths (PWM demo vs network decode) without confusing the user.

Until Phase 0 passes, Opus-on-audio-lab is **premature**; Audio lab can remain PWM demo until a **single** network decode path is proven.

## Honest answer to “is it really hard?”

- **No** if you accept **months** of incremental work, **one codec at a time**, and **extract** a thin shared playout layer from desktop lessons.
- **Yes** if you expect **drop-in libopus + full matrix** next to CDG on first silicon without Phase 0–1 discipline.

When Phases 0–1 are green on your desk, say the word and treat **AMR-WB + CDG soak** as the first “real” soak campaign; Opus stays last per your priority and per embedded headroom.
