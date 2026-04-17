# Test plan: v4 transport reliability, codec switching, and load behaviour

## Scope

Validate **codec hot-swap** without TX restart, **recovery** when **`v4_session_info` is dropped** but **`v4_audio_chunk`** continues, **audio continuity** under host CPU/disk load, and **pause/unpause** without wedging.

## Preconditions

- Two hosts or loopback; **v4** session; non-retro TX/RX builds with multiple codecs (e.g. AMR-WB ↔ Opus ↔ SBC-like).
- Tools: Task Manager / `stress` disk / parallel `make -j` to simulate load.

## Automated / unit

- `make test` — existing protocol and core tests must pass after changes.
- FEC xor recovery remains covered by existing `proto`/FEC tests where applicable.

## Manual QA

### 1. Codec cycling (TTY `c` on TX)

1. Start TX + RX; confirm audio and CDG.
2. Press **`c`** repeatedly (at least **5** cycles through the list).
3. **Expect**: RX stays audible after **every** press; no **permanent silence** until TX restart.
4. **Regression check**: if silence occurs, capture whether **`v4_session_info`** appears in a capture (Wireshark) before first new **`v4_audio_chunk`**.

### 2. Simulated lost session_info

1. Use a firewall or temporary drop rule to block **only** `v4_session_info` (type 13) **or** use a modified client that drops first N session_info packets after codec change (advanced).
2. **Expect**: Once **`v4_audio_chunk`** with new `codec_id` arrives, RX **reconciles** and audio resumes without full TX restart (may glitch briefly).

### 3. Host load

1. While streaming, run **parallel compilation**, **large file copy**, and **browser** activity.
2. **Expect**: Occasional **crackle** acceptable; **no permanent dropout**; pause/unpause **usually** recovers in **one** cycle after fixes (multi-cycle recovery documented as residual risk until MMCSS / priority work).

### 4. Pause / unpause

1. Toggle pause on TX several times during playback.
2. **Expect**: Audio and subtitle sync recover; jitter counters in HUD (if enabled) do not grow unbounded.

### 5. Legacy RX (optional)

1. Run **retro RX** against modern TX (supported codecs only).
2. **Expect**: CPU remains low; video sync holds per existing retro baselines.

## Exit criteria

- Manual cases **1** and **3** pass on a **Windows** reference machine.
- No known **silent-until-restart** path from codec hot-swap alone.

## References

- [`../specs/v4-codec-switching-contract.md`](../specs/v4-codec-switching-contract.md)
- [`../specs/v4-transport-stability-and-timing.md`](../specs/v4-transport-stability-and-timing.md)
- [`../specs/v4-audio-fec-advanced.md`](../specs/v4-audio-fec-advanced.md)
