# V4 audio FEC — current XOR model and advanced options

## Current implementation (shipping)

- **Session**: `audio_join_redundancy` and repair mode describe **startup** and **FEC group** behaviour; see `DASHCDG_PACKET_FEC_PARITY` and `proto/src/fec.c` (**xor** across payloads in a group, **one** parity per group).
- **Transmitter** emits parity **after** the group’s frames (standard **erasure** pattern: parity **lags** data so a single loss in the group can be recovered when the parity arrives).
- **Receiver** tracks per-group payloads and applies `dashcdg_fec_parity_recover` when exactly **one** member is missing and parity is present.

This matches common **real-time** practice: **parity is not “inside”** the previous media packet on the wire today; it is a **separate small datagram** keyed by `group_id` / `stream_type`.

## “Next in previous” / piggyback (not default on wire today)

Some systems **embed** a **redundant copy** or **xor shell** of the **next** frame in the **current** packet to avoid an extra datagram. That **changes** `v4_audio_chunk` layout or adds a **new packet type** / **version flag**.

**Roadmap criteria** (only with a **protocol bump** and compatibility plan):

1. **Bandwidth**: Piggyback increases every audio packet size; must stay under **path MTU** and `DASHCDG_MAX_PACKET_SIZE`.
2. **Latency**: Carrying **N+1** in packet **N** adds **one frame** of coding delay unless combined with overlapping XOR.
3. **Codec change**: Any piggyback scheme must **reset** with **session_info** / **codec_id** boundaries.

Until a versioned payload exists, **do not** claim piggyback is active; the **documented** approach remains **separate FEC parity packets** plus **receiver reconciliation** on loss and **codec** switches.

## Testing

See [`../test/v4-transport-reliability-validation.md`](../test/v4-transport-reliability-validation.md).
