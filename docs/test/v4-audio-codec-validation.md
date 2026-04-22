# V4 audio codec — validation checklist

Manual and automated checks for v4 audio codec selection and session metadata.

## Automated (core / protocol)

- `make test` (or CI equivalent): `tests/test_core.c` round-trips **v4 session info** including `song_id` and each **audio_codec_id** value used on the wire (at minimum: `OPUS`, `SBC_LIKE`, `CELP13K`, `QCELP8K`, `AMR_NB`, `AMR_WB`, `BLUETOOTH_SBC`).
- `build/bin/test-opus-roundtrip` (when built) — `dashcdg_opus_*` encode/decode sine at **64 kbit/s**; guards Opus regression (`tests/test_opus_roundtrip.c`).
- After running **`scripts/fetch_audio_codec_vendors.sh`**, add CI or local steps to verify **vendored `vendor/` trees** match pinned commits (optional submodule / lockfile — TBD).
- **`test_nb_ima_codec_roundtrip()`** — encode/decode one frame with **`dashcdg_nb_ima_*`** (`core/src/nb_ima_codec.c`); asserts fixed-point path stays linked in **`libdashcdg_core`**.
- Assert **parse → serialize → parse** preserves `song_id` nul-padded field semantics.

## Manual — TX CLI

1. **Opus (default quality path)**  
   `desktop-tx ... --v4-audio-codec=opus`  
   Expect: session info `audio_codec_id=1`, normal preroll/bitrate fields.

2. **Narrowband shim — `sbc-like`**  
   `--v4-audio-codec=sbc-like` + resilience (or `--audio-profile=resilience`).  
   Expect: `audio_codec_id=2`, receiver plays; `song_id` visible on RX (`[rx] asset ready for <name>`).

3. **`--badnet-v4`**  
   Expect: v4 enabled, resilience, **`amr-wb` id (6)** on wire; RX decodes with the AMR-WB desktop path.

4. **`--badnet-v4-sbc` / `--badnet-v4-qcelp8k`**  
   Expect ids **2** and **4** respectively.

5. **Native narrowband ids (3–7)**  
   `--v4-audio-codec=amr-nb`, `amr-wb`, `bluetooth-sbc`, `qcelp8k`, `celp13k` — expect distinct native codec payloads and successful RX decode / reconfigure without reverting to the NB-IMA path. Legacy `evrc` remains acceptable only as an alias for the id-4 path.

## Manual — RX

- Join v4 session; confirm **song title** matches TX `song-id` / library metadata after **first v4 session info** (not `<unknown>`).
- Switch TX codec between two narrowband ids; confirm RX **reconfigures** without crash (may reset audio pipeline).
- While TX is running steadily with one codec, leave RX up for at least 2 minutes and confirm periodic `v4_session_info` does **not** repeatedly reset the output device, drop back into `wait-preroll`, or flush buffered audio.
- Switch TX codec with TTY `c`; confirm RX either follows the preceding `v4_session_info` or reconciles from the first `v4_audio_chunk.codec_id` if session_info is delayed/lost, and does not stay silent on the old decoder.

## Future — native codec rows

When replacing shim for an id:

- [ ] Byte-exact frame size ≤ `DASHCDG_MAX_AUDIO_FRAME_BYTES` (255).
- [ ] TX and RX agree on **codec_id** and **encoded_length** per frame.
- [ ] Fuzz / malformed payload handling (drop frame, count `audio_decode_failures`).
- [ ] Win32 retro / legacy CPU builds: no SSE2 in codec object code (separate build matrix doc).
