# Karaoke Transport Protocol v2

## Goals

- Wi-Fi-first, UDP multicast-friendly transport
- portable binary framing for desktop and future embedded receivers
- live network audio plus live timed CD+G packets
- late-join support without requiring retransmit
- explicit versioning so protocol evolution stays parseable

## Packet framing

Every datagram starts with the fixed header defined in `proto/include/dashcdg/protocol.h`.

Header fields:

- `magic`: `DKG1`
- `version`: protocol version, currently `2`
- `type`: packet discriminator
- `flags`: transport flags; today `DASHCDG_PACKET_FLAG_PAUSED` is used to advertise paused playback state
- `sequence`: sender-wide monotonically increasing datagram sequence
- `sender_time_ms`: sender monotonic time in milliseconds
- `payload_length`: payload size in bytes
- `reserved`: currently unused

Important framing constants:

- `DASHCDG_MAX_PACKET_SIZE = 1400`
- `DASHCDG_MAX_ASSET_CHUNK = 1024`
- `DASHCDG_MAX_AUDIO_FRAME_BYTES = 255`
- `DASHCDG_MAX_CDG_BATCH_PACKETS = 6`
- `DASHCDG_MAX_FEC_PAYLOAD_BYTES = 255`
- `DASHCDG_SUBCHANNEL_PACKET_BYTES = 24`

## Packet types

### `ANNOUNCE`

Purpose:

- session discovery
- late-join session metadata
- live media configuration

Fields:

- `song_id`
- `asset_size`
- `chunk_size`
- `packets_per_second`
- `audio_sample_rate`
- `audio_channels`
- `audio_frame_ms`
- `audio_bitrate_kbps`
- `playout_delay_ms`
- `audio_fec_group_size`
- `cdg_fec_group_size`
- `session_start_ms`

Current desktop TX behavior:

- always sent periodically
- advertises live audio metadata when the current track has an `.mp3`
- advertises `0` audio fields for CDG-only tracks

### `ASSET_CHUNK`

Purpose:

- late-join bootstrap of the full CD+G asset

Fields:

- `asset_offset`
- `chunk_length`
- `chunk_bytes`

Current desktop behavior:

- TX repeatedly loops the entire CD+G file in 1024-byte slices
- RX accepts out-of-order and duplicate chunks
- RX declares `ready` once it has the full asset and can seek deterministically

### `CLOCK_BEACON`

Purpose:

- coarse session/playback update
- late-join asset availability reporting

Fields:

- `song_id`
- `session_start_ms`
- `playback_ms`
- `available_asset_bytes`
- `total_asset_bytes`

Current desktop behavior:

- TX emits frequent beacons during playback
- RX uses `sender_time_ms` plus the local offset estimator more heavily than the beacon `playback_ms` field

### `AUDIO_FRAME`

Purpose:

- live Opus audio transport

Fields:

- `media_sequence`
- `group_id`
- `group_index`
- `frame_ms`
- `encoded_length`
- `playback_ms`
- `encoded_bytes`

Current desktop behavior:

- TX pre-encodes the source `.mp3` into 48 kHz stereo Opus frames
- TX sends 20 ms audio frames
- RX decodes frames into a queue-driven PortAudio stream
- RX uses the announced playout delay to decide when to start the stream

### `CDG_BATCH`

Purpose:

- live timed CD+G transport

Fields:

- `media_sequence`
- `group_id`
- `group_index`
- `packet_count`
- `reserved`
- `packet_start_index`
- `packet_bytes`

Current desktop behavior:

- each batch carries up to 6 raw CD+G subchannel packets
- TX timestamps batches by the source CD+G packet index converted into milliseconds
- RX applies batches in order to a live `dashcdg_cdg_state`

### `PTP_SYNC`

Purpose:

- sender clock sync marker

Fields:

- `sync_id`
- `reserved`

Current desktop behavior:

- emitted by TX
- observed by RX to refine the remote/local offset estimate

### `PTP_FOLLOW_UP`

Purpose:

- send the explicit origin timestamp associated with a sync marker

Fields:

- `sync_id`
- `reserved`
- `origin_time_ms`

Current desktop behavior:

- emitted by TX immediately after `PTP_SYNC`
- observed by RX

### `PTP_DELAY_REQ`

Purpose:

- reserved for future round-trip path-delay measurement

Fields:

- `request_id`
- `reserved`

Current implementation status:

- parser/serializer exists
- desktop proof does not emit or consume it in the media loop

### `PTP_DELAY_RESP`

Purpose:

- reserved for future reply carrying receiver-observed timing

Fields:

- `request_id`
- `reserved`
- `request_rx_time_ms`

Current implementation status:

- parser/serializer exists
- desktop proof does not emit or consume it in the media loop

### `FEC_PARITY`

Purpose:

- reserved for future bounded parity repair

Fields:

- `stream_type`
- `group_size`
- `payload_bytes`
- `reserved`
- `group_id`
- `payload_length_xor`
- `reserved_b`
- `payload_xor`

Current implementation status:

- parser/serializer exists
- no parity packets are currently generated or recovered by the desktop apps

## Sender defaults in the current desktop proof

- audio sample rate: `48000`
- channels: `2`
- Opus frame duration: `20 ms`
- Opus bitrate target: `96 kbps`
- announced playout delay: `500 ms`
- announced audio FEC group size: `5`
- announced CDG FEC group size: `9`
- CD+G cadence: `300 packets/second`

## Receiver behavior

The current desktop receiver uses both bootstrap and live paths:

1. `ANNOUNCE` prepares or resets session state
2. `ASSET_CHUNK` rebuilds the full CD+G asset for deterministic late join
3. `CLOCK_BEACON`, `PTP_SYNC`, and `PTP_FOLLOW_UP` maintain a bounded sender/local offset estimate
4. `AUDIO_FRAME` feeds the Opus decoder and PortAudio queue
5. `CDG_BATCH` advances the live CD+G state

If a local MP3 path is provided on RX startup, the legacy local-file audio fallback path still exists. If `ANNOUNCE` advertises audio metadata, RX prefers the network Opus streaming path.

## Late-join behavior

The current proof supports late join by combining:

1. repeated `ANNOUNCE`
2. continuous asset replay through `ASSET_CHUNK`
3. periodic `CLOCK_BEACON`
4. ongoing live `AUDIO_FRAME` and `CDG_BATCH` transmission

In practice, RX can start live audio before the bootstrap asset is complete, but `ready` is only asserted once the full CD+G asset has been rebuilt.

## Known protocol-proof limitations

- no authenticated control plane
- no retransmit, NACK, or repair-request path
- no playlist/session catalog packet family
- no full PTP delay-request/response discipline yet
- no active `FEC_PARITY` generation or recovery yet
- no wire-level compatibility promise for protocol v1 peers

These are intentional current omissions, not undocumented behavior.
