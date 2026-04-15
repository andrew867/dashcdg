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

Candidate codec directions to evaluate in implementation:

- lower-rate Opus
- `mu-law` or `A-law`
- a simple SBC-like low-bitrate framed mode
- another codec that has both floating-point desktop and fixed-point MCU paths

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

## Profile Selection Policy

The operator-facing runtime should eventually support:

- explicit profile selection
- a default quality profile
- an easy way to force resilience mode for field testing

Automatic profile switching is not required in the first tranche. It may be
added later, but the wire format should not prevent it.

## Proof Requirements

`quality` mode must prove:

- acceptable startup on healthy networks
- no regression versus the current desktop proof for normal venue use

`resilience` mode must prove:

- materially lower link budget
- successful late-join audio startup under the weak-Wi-Fi validation matrix
- explicit operator-visible indication that the resilience profile is active
