# Audio jitter buffer and playout boundary (specification)

## Document control

| Field | Value |
| --- | --- |
| Scope | Receiver audio reorder / de-jitter **before** device queue |
| Location | `core/include/dashcdg/audio_jitter.h`, `core/src/audio_jitter.c` |
| Dependencies | **No** `PortAudio`, **no** `Opus`, **no** sockets — only C99 and `dashcdg/common.h` size constants |

## Purpose

Extract the **pure state machine** for:

- bounded slot storage of encoded audio frames keyed by `media_sequence`
- insertion rules (late duplicate drop, reorder counters)
- ordered release for the next expected sequence
- late join / missing-frame skip policy inputs supplied by the caller

**Playout** (decode Opus/SBC-like → PCM → `dashcdg_desktop_audio_queue_frames`) **remains** in `app_rx.c` but **MUST** consume frames only through the jitter API once wired, so that:

- golden tests can validate jitter behavior without audio devices
- ESP-IDF can link the same core object and swap device queue for I2S

## Data model

### Slot

```c
#define DASHCDG_AUDIO_JITTER_MAX_PAYLOAD 255u  /* MUST match DASHCDG_MAX_AUDIO_FRAME_BYTES */

struct dashcdg_audio_jitter_frame {
    int occupied;
    uint32_t media_sequence;
    uint64_t playback_ms;
    uint8_t frame_ms;
    uint8_t audio_profile_id;
    uint8_t codec_id;
    uint16_t encoded_length;
    uint8_t encoded_bytes[DASHCDG_AUDIO_JITTER_MAX_PAYLOAD];
};
```

### Buffer

```c
struct dashcdg_audio_jitter_buffer {
    struct dashcdg_audio_jitter_frame slots[DASHCDG_AUDIO_JITTER_SLOT_COUNT];
    int initialized;
    uint32_t next_media_sequence;
    uint64_t next_playback_ms;
    uint64_t reordered_packets;
    uint64_t pending_drops;
};
```

`DASHCDG_AUDIO_JITTER_SLOT_COUNT` **MUST** equal the former `DASHCDG_AUDIO_JITTER_BUFFER_PACKETS` (`64`).

## API (normative)

| Function | Behavior |
| --- | --- |
| `void dashcdg_audio_jitter_init(struct dashcdg_audio_jitter_buffer *jb)` | Zero all slots; `initialized = 0`. |
| `void dashcdg_audio_jitter_clear(struct dashcdg_audio_jitter_buffer *jb)` | Same as init. |
| `int dashcdg_audio_jitter_insert(struct dashcdg_audio_jitter_buffer *jb, uint32_t media_sequence, uint64_t playback_ms, uint8_t frame_ms, uint8_t audio_profile_id, uint8_t codec_id, const uint8_t *payload, uint16_t payload_length, int count_stats)` | Same semantics as legacy `dashcdg_rx_insert_audio_pending_locked` for ordering and slot allocation. Returns `1` on store, `0` on drop. |
| `struct dashcdg_audio_jitter_frame *dashcdg_audio_jitter_find(const struct dashcdg_audio_jitter_buffer *jb, uint32_t media_sequence)` | Pointer to internal slot or `NULL`. |
| `struct dashcdg_audio_jitter_frame *dashcdg_audio_jitter_oldest(const struct dashcdg_audio_jitter_buffer *jb)` | Lowest `media_sequence` among occupied slots. |
| `size_t dashcdg_audio_jitter_occupied_count(const struct dashcdg_audio_jitter_buffer *jb)` | Count occupied. |
| `void dashcdg_audio_jitter_release_slot(struct dashcdg_audio_jitter_buffer *jb, struct dashcdg_audio_jitter_frame *slot)` | Clears `occupied` for a slot belonging to `jb`. |
| `enum dashcdg_audio_drain_step dashcdg_audio_jitter_drain_step(struct dashcdg_audio_jitter_buffer *jb, const struct dashcdg_audio_jitter_drain_input *in, struct dashcdg_audio_jitter_frame **out_frame, uint64_t *out_missing_skips_delta)` | One iteration of the former `while` body in `dashcdg_rx_drain_media_locked` for **audio only**. |

### Drain input (caller supplies playout gating)

```c
struct dashcdg_audio_jitter_drain_input {
    int have_sender_playback;
    uint64_t sender_playback_now_ms;
    uint8_t announced_audio_frame_ms;
    uint16_t announced_playout_delay_ms;
    int audio_stream_started;       /* former g_audio_stream_started */
    int audio_device_null;        /* g_audio == NULL */
    uint32_t audio_buffered_ms;   /* device queue depth in ms */
};
```

### Drain result

```c
enum dashcdg_audio_drain_step {
    DASHCDG_AUDIO_DRAIN_STOP = 0,   /* no work this tick */
    DASHCDG_AUDIO_DRAIN_APPLY,      /* *out_frame points at slot to decode+queue; caller MUST release after success */
    DASHCDG_AUDIO_DRAIN_SKIP        /* gap handling; *out_missing_skips_delta updated */
};
```

**APPLY path:** Caller decodes and queues PCM; on **successful** queue, caller **MUST** call `dashcdg_audio_jitter_note_applied(jb, frame, frame_ms_effective)` which bumps `next_media_sequence` and `next_playback_ms` and clears the slot.

**SKIP path:** Jitter updates `next_media_sequence` / `next_playback_ms` exactly as legacy skip branches; `*out_missing_skips_delta` is the increment for RX aggregate stats (0 or gap size).

**Empty-buffer skip guard:** When the next `media_sequence` is missing and **no** buffered frame exists ahead of it (`dashcdg_audio_jitter_oldest` would advance reorder only when `oldest->media_sequence > next`), advancing the sequence without a queued packet is allowed only if **sender playback minus `jb->next_playback_ms` ≥ `DASHCDG_AUDIO_SKIP_EMPTY_MIN_SKEW_MS`** (see `audio_jitter.h`). Otherwise return **STOP** so join skew between `clock_sync` and drained packet timestamps cannot ghost-skip in-flight frames (regression: one blip of audio then silence).

**Playout-delay alignment:** The drain step must compare `jb->next_playback_ms` against the **receiver local playout target**, not raw sender playback. Implementations therefore subtract `announced_playout_delay_ms` from `sender_playback_now_ms` before deciding a frame is late. Without that subtraction, the receiver treats every packet as late by roughly the entire preroll and burns through startup audio immediately.

**Skip-starvation bypass:** The drain input’s `audio_buffered_ms` gate avoids skipping while the host PCM ring is still “deep.” If `ms_since_prior_audio_apply` exceeds `DASHCDG_AUDIO_STARVATION_GATE_BYPASS_MS` (~900 ms), the gate opens anyway so loss recovery cannot deadlock when the ring is stuck high but decode has stalled (see `docs/specs/desktop-rx-p3-gdi-audio-stall-rca.md`).

## Invariants (MUST hold)

1. `payload_length <= DASHCDG_AUDIO_JITTER_MAX_PAYLOAD` or insert returns `0`.
2. Insert never stores duplicate `media_sequence`.
3. `find(jb, next_media_sequence)` after successful apply returns `NULL` for that sequence.
4. Drain **never** calls audio device APIs.

## Relationship to FEC

FEC recovery in `app_rx.c` **MAY** call `dashcdg_audio_jitter_insert` after reconstructing a payload. No FEC logic lives inside `audio_jitter.c`.

## Versioning

Any change to reorder or skip policy **MUST** update:

- this spec
- `docs/test/audio-jitter-playout-validation.md`
- core unit tests
