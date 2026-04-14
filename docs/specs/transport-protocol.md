# Karaoke Transport Protocol v1

## Goals

- Wi-Fi first
- UDP multicast-friendly
- desktop and ESP-IDF compatible
- late-join capable
- versioned binary framing

## Packet framing

Every datagram begins with the fixed header defined in `proto/include/dashcdg/protocol.h`.

Header fields:

- `magic`: `DKG1` magic number
- `version`: protocol version
- `type`: announce, asset chunk, clock beacon, or control
- `sequence`: monotonically increasing sender sequence number
- `sender_time_ms`: sender monotonic time in milliseconds
- `payload_length`: payload size in bytes

## Packet types

### `ANNOUNCE`

Sent periodically so a late receiver can discover:

- `song_id`
- total asset size
- expected chunk size
- CD+G packet cadence
- planned session start time

### `ASSET_CHUNK`

Carries a slice of the CD+G asset:

- `asset_offset`
- `chunk_length`
- raw bytes

Receivers must allow out-of-order arrival and repeated chunks.

### `CLOCK_BEACON`

Sent frequently to discipline receiver clocks:

- `song_id`
- `session_start_ms`
- current `playback_ms`
- asset availability counters

The current desktop receiver trusts `sender_time_ms` more than `playback_ms` for clock discipline, then derives `playback_ms` from the disciplined sender clock and the advertised session start.

## Late join behavior

The current proof implementation supports late join by:

1. repeated `ANNOUNCE`
2. continuous round-robin asset chunk replay
3. periodic `CLOCK_BEACON`

## Known v1 limitations

- no authenticated control plane yet
- no retransmission requests yet
- no negative acknowledgements yet
- no song playlist/session catalog yet
- no FEC yet

These are intentional omissions for the desktop proof tranche.
