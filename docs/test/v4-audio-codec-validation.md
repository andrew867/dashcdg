# V4 audio codec — validation checklist

Manual and automated checks for v4 audio codec selection and session metadata.

## Automated (core / protocol)

- `make test` (or CI equivalent): `tests/test_core.c` round-trips **v4 session info** including `song_id` and each **audio_codec_id** value used on the wire (at minimum: `OPUS`, `SBC_LIKE`, `CELP13K`, `EVRC`, `AMR_NB`, `AMR_WB`, `BLUETOOTH_SBC`).
- Assert **parse → serialize → parse** preserves `song_id` nul-padded field semantics.

## Manual — TX CLI

1. **Opus (default quality path)**  
   `desktop-tx ... --v4-audio-codec=opus`  
   Expect: session info `audio_codec_id=1`, normal preroll/bitrate fields.

2. **Narrowband shim — `sbc-like`**  
   `--v4-audio-codec=sbc-like` + resilience (or `--audio-profile=resilience`).  
   Expect: `audio_codec_id=2`, receiver plays; `song_id` visible on RX (`[rx] asset ready for <name>`).

3. **`--badnet-v4`**  
   Expect: v4 enabled, resilience, **`celp13k` id (3)** on wire; RX decodes with narrowband shim.

4. **`--badnet-v4-sbc` / `--badnet-v4-evrc`**  
   Expect ids **2** and **4** respectively.

5. **AMR / Bluetooth SBC ids**  
   `--v4-audio-codec=amr-nb`, `amr-wb`, `bluetooth-sbc` — expect **same shim audio** as `sbc-like` until native encoders land; use for **interop testing** of session announce + chunk routing only.

## Manual — RX

- Join v4 session; confirm **song title** matches TX `song-id` / library metadata after **first v4 session info** (not `<unknown>`).
- Switch TX codec between two narrowband ids; confirm RX **reconfigures** without crash (may reset audio pipeline).

## Future — native codec rows

When replacing shim for an id:

- [ ] Byte-exact frame size ≤ `DASHCDG_MAX_AUDIO_FRAME_BYTES` (255).
- [ ] TX and RX agree on **codec_id** and **encoded_length** per frame.
- [ ] Fuzz / malformed payload handling (drop frame, count `audio_decode_failures`).
- [ ] Win32 retro / legacy CPU builds: no SSE2 in codec object code (separate build matrix doc).
