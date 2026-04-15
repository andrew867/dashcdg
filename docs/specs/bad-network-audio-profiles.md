# Bad-Network Audio Profiles

## Purpose

This document defines the audio-profile layer for the bad-network transport
tranche described in `docs/specs/bad-network-transport.md`.

The receiver must be able to late join and initialize decode immediately from
wire metadata alone.

## Profiles

### `quality`

Purpose:

- normal venue playback on healthy Ethernet or strong Wi-Fi
- retained higher-quality path for the desktop proof's intended experience

Target behavior:

- preserve the current live-audio feel
- tolerate moderate reorder and occasional isolated loss
- accept somewhat higher bitrate than the resilience profile

Starting direction:

- retain Opus
- keep `20 ms` packetization unless testing proves a different frame size is
  materially more resilient
- allow in-band or transport-level repair, but do not assume heavy startup
  bursts

First-tranche lock:

- codec family: Opus
- network sample rate: `48 kHz`
- channels: mono for the desktop network path
- nominal frame size: `20 ms`
- nominal bitrate target: start from the current desktop proof range and carry
  the exact configured bitrate on the wire
- startup policy: light join redundancy, then steady-state paced send

### `resilience`

Purpose:

- weak Wi-Fi survival mode
- lab and MCU-oriented testing mode
- deterministic low-bitrate comparison point when debugging startup failures

Target behavior:

- lower steady-state bitrate than `quality`
- smaller join burst cost
- simpler decode path where practical
- better tolerance of startup packet loss through redundancy or shorter decoder
  dependency chains

First-tranche lock:

- codec family: SBC-like framed low-bitrate mode
- target purpose: weak-link survival and deterministic debugging profile
- expected properties:
  - materially lower steady-state bitrate than `quality`
  - short independent frames with shallow dependency chains
  - simple enough decode path that a future MCU receiver is plausible
  - explicit join redundancy for the first audio groups

## Wire Signaling

Every session announcement for the new transport must expose:

- `audio_profile_id`
- `codec_id`
- `sample_rate`
- `channels`
- `frame_ms`
- `bitrate_or_mode`
- `join_redundancy_mode`
- `fec_or_repair_mode`

For the first v4 rollout, the metadata should be concrete enough that RX can
instantiate either decoder immediately:

- `audio_profile_id = quality | resilience`
- `codec_id = opus | sbc_like`
- `sample_rate`
- `channels`
- `frame_ms`
- `bitrate_or_mode`
- `join_redundancy_mode`
- `fec_or_repair_mode`
- `startup_preroll_ms`

Late-join receivers must not need out-of-band configuration to start decoding.

## Startup Rules

The audio-profile layer must support different startup behavior from steady
state.

Required concepts:

- join-time burst policy for the first audio groups
- profile-aware preroll target
- explicit fallback if the first groups are incomplete
- observability that states whether RX is waiting on packets, decoder config,
  preroll depth, or repair completion

First-tranche startup policy:

- `quality`
  - modest join redundancy for the first audio groups
  - preserve the current desktop feel on healthy links
  - prefer Opus continuity over aggressive startup duplication
- `resilience`
  - duplicate or otherwise redundantly send the first few audio groups
  - use a smaller, simpler frame format to reduce dependency on one perfect
    initial burst
  - expose a lower startup preroll target than `quality` if testing supports it

## Profile Selection Policy

The operator-facing runtime should eventually support:

- explicit profile selection
- a default quality profile
- an easy way to force resilience mode for field testing

Automatic profile switching is not required in the first tranche. It may be
added later, but the wire format should not prevent it.

First-tranche operator policy:

- default to `quality`
- allow an explicit forced `resilience` mode in TX
- make the active mode visible in both TX and RX status output

Current TX status:

- TX now accepts `--audio-profile=quality|resilience` on the opt-in bad-network
  v4 path
- TX status/event output reports the active `profile` and `codec`

## Proof Requirements

`quality` mode must prove:

- acceptable startup on healthy networks
- no regression versus the current desktop proof for normal venue use

`resilience` mode must prove:

- materially lower link budget
- successful late-join audio startup under the weak-Wi-Fi validation matrix
- explicit operator-visible indication that the resilience profile is active

The first implementation should additionally prove:

- decoder bring-up from wire metadata alone for both codecs
- recovery from damage to the first audio groups without a permanent
  video-only-started / audio-never-started wedge
