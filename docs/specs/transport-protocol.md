# Karaoke Transport Protocol v3

This document describes the current protocol v3 desktop proof. The separate
bad-network redesign tranche lives in `docs/specs/bad-network-transport.md` and
is allowed to introduce a future protocol v4 or equivalent wire break.

The TX CD+G source/memory slimdown work is documented separately in
`docs/specs/tx-cdg-source-model.md`. That tranche is intended to preserve the
current packet families while changing how TX stores and reads CD+G data.

## Goals

- Wi-Fi-first, UDP multicast/broadcast-friendly transport
- portable binary framing for desktop and future embedded receivers
- live network audio plus live timed CD+G packets
- late-join support without requiring retransmit
- explicit pause-state signaling and fast video recovery anchors
- explicit versioning so protocol evolution stays parseable

## Packet framing

Every datagram starts with the fixed header defined in `proto/include/dashcdg/protocol.h`.

Header fields:

- `magic`: `DKG1`
- `version`: protocol version, currently `3`
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
- `DASHCDG_MAX_CDG_SNAPSHOT_CHUNK = 1024`
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
- RX treats a fresh announce as a session re-anchor on track/session changes so playout does not wait for slow clock re-convergence alone

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
- current TX sources those slices from a random-access CDG source
- headless/default TX may use a file-backed source; preview mode still has a
  memory-backed fallback

Slimdown target:

- `ASSET_CHUNK` stays byte-addressed on the wire
- TX should eventually stop needing the preview-only whole-memory fallback for
  deterministic local preview/seek behavior

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
- TX also emits paused-state beacons during pause mode so RX can keep a healthy session view without advancing media

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

- TX produces 48 kHz mono Opus frames incrementally on a dedicated audio thread
- a bounded `audio_ready_queue` is the handoff boundary between audio
  production and packet pacing
- TX sends 20 ms audio frames
- RX decodes frames into a queue-driven PortAudio stream
- RX uses the announced playout delay to decide when to start the stream
- desktop RX network mode uses network audio only; it no longer falls back to local-file MP3 playout

Threaded-runtime target:

- TX audio production moves to incremental decode/resample/encode rather than whole-track prebuild
- `media_sequence`, `group_id`, and `group_index` remain strictly monotonic on the wire even when frames are produced just ahead of playout deadlines
- bounded producer queues, not whole-track arrays, become the pacing contract between TX audio production and TX network send

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
- current TX prebuilds all batches for a track in `cdg_batches`

Stage A implementation note:

- `cdg_batches` now stores metadata only; payload bytes are read from the
  canonical CDG source at send/FEC time

Threaded-runtime target:

- live CDG batches are produced from a timeline-driven video task and handed to the network sender through a bounded queue or equivalent publish boundary
- RX video progression and RX bootstrap progression become independent subsystems

Slimdown target:

- batch scheduling metadata can stay precomputed, but payload bytes should be
  read from the canonical CDG source at send time
- later source work should make file-backed and memory-backed paths equally
  deterministic for all TX use cases, including preview

### `CDG_SNAPSHOT`

Purpose:

- bounded late-join video state keyframe
- immediate live framebuffer bootstrap before the full asset is rebuilt

Fields:

- `snapshot_id`
- `packet_index`
- `total_bytes`
- `snapshot_offset`
- `chunk_length`
- `snapshot_bytes`

Current desktop behavior:

- TX periodically serializes the current live `dashcdg_cdg_state` into bounded 1024-byte chunks
- TX aligns each snapshot to the next live `CDG_BATCH` packet index boundary
- RX reassembles the snapshot, restores the live framebuffer/palette state, and resumes from the aligned live `CDG_BATCH` packet index
- the full `ASSET_CHUNK` replay still continues in parallel so deterministic seek/bootstrap completes in the background
- RX can also apply snapshots mid-session while paused or recovering, not just before bootstrap completion

Threaded-runtime target:

- snapshots are a recovery and fast-start input to the RX video task
- snapshot failure or rejection must not permanently block later live video progression

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
- RX discards stale sync exchanges that exceed the current bounded age window rather than poisoning the clock estimate after delayed track loads

### `PTP_DELAY_REQ`

Purpose:

- reserved for future round-trip path-delay measurement

Fields:

- `request_id`
- `reserved`

Current implementation status:

- parser/serializer exists
- RX now emits it after matching `PTP_SYNC` plus `PTP_FOLLOW_UP`
- TX listens for it and answers with `PTP_DELAY_RESP`

### `PTP_DELAY_RESP`

Purpose:

- reserved for future reply carrying receiver-observed timing

Fields:

- `request_id`
- `reserved`
- `request_rx_time_ms`

Current implementation status:

- parser/serializer exists
- TX emits it in response to `PTP_DELAY_REQ`
- RX consumes it and updates sender offset/path-delay estimates
- TX now timestamps the response after acquiring its main mutex, reducing stale timing caused by track-load stalls
- RX discards stale delayed responses that arrive outside the bounded exchange window

### `FEC_PARITY`

Purpose:

- bounded parity repair for short audio and CD+G media groups

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
- TX emits one XOR parity packet per completed audio/CD+G FEC group
- RX stores short per-group history and can recover a single missing media payload when parity and the other group members arrive

## Sender Defaults In The Current Desktop Proof

- audio sample rate: `48000`
- channels: `1`
- Opus frame duration: `20 ms`
- Opus bitrate target: `80 kbps`
- announced playout delay: `500 ms`
- announced audio FEC group size: `5`
- announced CDG FEC group size: `9`
- CD+G cadence: `300 packets/second`
- TX warmup before a new session: `1000 ms`
- CDG snapshot interval: `1000 ms`

Desktop endpoint defaults:

- TX and RX default to `239.255.77.77:24684`
- desktop tools also accept explicit IPv4 broadcast endpoints such as `192.168.0.255`

## Receiver behavior

The current desktop receiver uses both bootstrap and live paths:

1. `ANNOUNCE` prepares or resets session state
2. `ASSET_CHUNK` rebuilds the full CD+G asset for deterministic late join
3. `CLOCK_BEACON`, `PTP_SYNC`, `PTP_FOLLOW_UP`, `PTP_DELAY_REQ`, and `PTP_DELAY_RESP` maintain a bounded sender/local offset estimate and path-delay estimate
4. `AUDIO_FRAME` feeds the Opus decoder and PortAudio queue
5. `CDG_BATCH` advances the live CD+G state
6. `CDG_SNAPSHOT` can seed or replace the current live visual state
7. `FEC_PARITY` can recover a single missing media payload inside a protected group

RX startup is now network-audio-only for desktop network mode. When `ANNOUNCE` advertises audio metadata, RX initializes the Opus/PortAudio path and keeps a bounded pending queue for reordered `AUDIO_FRAME` and `CDG_BATCH` packets before declaring them late. Current desktop HUD/stdout observability also exposes startup gate state, clock step/peak/holdover data, and current FEC repair hotness.

Threaded-runtime target:

1. socket receive parses packets and publishes typed packet events without waiting on render/UI work
2. PTP observations feed a dedicated clock-discipline task
3. bootstrap replay, audio progression, and live video progression advance independently
4. render consumes immutable published frame snapshots rather than mutating receiver transport state directly

## Late-join behavior

The current proof supports late join by combining:

1. repeated `ANNOUNCE`
2. continuous asset replay through `ASSET_CHUNK`
3. periodic `CLOCK_BEACON`
4. periodic `CDG_SNAPSHOT` state keyframes
5. ongoing live `AUDIO_FRAME` and `CDG_BATCH` transmission

In practice, RX can start live audio before the bootstrap asset is complete, can show live video from a snapshot before deterministic asset rebuild is done, and only asserts full `ready` once the complete CD+G asset has been rebuilt.

## Known protocol-proof limitations

- no authenticated control plane
- no retransmit, NACK, or repair-request path
- no playlist/session catalog packet family
- no hardware-timestamp or sub-millisecond PTP discipline yet; current proof uses millisecond software timestamps
- bounded XOR FEC only repairs a single missing media payload per group; it is not a long-window or burst-loss code
- audio recovery still relies on playout delay and bounded FEC rather than a separate audio-state snapshot/keyframe model
- no wire-level compatibility promise for protocol v1 peers
- the threaded-runtime queue ownership model is a desktop architecture refactor and does not change protocol v3 packet formats by itself
- TX no longer requires a whole-memory `.cdg` preload for default wire send;
  Stage A and the initial source abstraction are in place, but preview-mode
  memory fallback and later source-layer polish remain tracked in
  `docs/specs/tx-cdg-source-model.md`
- the bad-network redesign work now intentionally targets a separate transport tranche rather than stretching v3 indefinitely

These are intentional current omissions, not undocumented behavior.
