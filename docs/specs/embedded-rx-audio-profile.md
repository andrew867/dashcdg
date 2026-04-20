# Embedded RX Audio Profile

## Purpose

Define how the first embedded receiver should treat audio while the bad-network
transport work is moving toward:

- `quality` audio backed by `Opus` (optional on MCU; desktop uses libopus)
- `resilience` audio backed by the **first-party fixed-point** narrowband codec
  (**`dashcdg_nb_ima_*`** in `core/`; CLI still exposes this family as `sbc-like`
  and related v4 wire labels — see [v4-audio-codecs.md](v4-audio-codecs.md))

This note is intentionally implementation-oriented for the first MCU receiver.
It does not replace `docs/specs/bad-network-audio-profiles.md`. It explains how
the embedded receiver should prioritize those profiles during bring-up.

### Desktop reference (2026)

**Windows desktop-rx / desktop-tx** now exercise the full **v4** codec matrix (Opus + ids **2–7**) with stable session_info reconfigure rules, codec hot-swap, cold join, and pause/resume behaviour — see [`../../AGENTS.md`](../../AGENTS.md), [`narrowband-low-bitrate-audio-quality.md`](narrowband-low-bitrate-audio-quality.md), and [`v4-codec-switching-contract.md`](v4-codec-switching-contract.md). Embedded bring-up should treat that stack as the **normative wire + timing contract**; MCU decode order and RAM gates remain per [`audio-codec-modules.md`](audio-codec-modules.md) and the ESP32 enterprise plan.

## First embedded policy

For the first ESP32 handheld receiver:

- `resilience` is the primary local-audio target
- `quality` is a later or optional target
- display-first fallback remains valid if local audio fails its feasibility gate

This policy matches the current repo direction:

- the bad-network work already treats the simpler low-bitrate path as the
  MCU-friendly profile
- the existing hardware notes already allow a display-first receiver before
  onboard audio is proven

## Why `resilience` comes first

The first embedded board has to share limited headroom across:

- Wi-Fi receive
- packet parsing
- startup state management
- visual rendering
- timing discipline
- local audio output

Because of that, the first embedded audio target should optimize for:

- simpler decode
- lower bitrate
- shorter dependency chains
- easier startup recovery

Those are the exact reasons the bad-network profile work defines `resilience`
around the **NB-IMA** low-bitrate mode (historically called “SBC-like” on the wire;
same codec for ESP32 bring-up without external speech libraries).

## Embedded audio priorities

### Priority 1: Audio existence

Prove that the board can:

- initialize decode from wire metadata alone
- produce audible output
- survive late join without wedging

### Priority 2: Startup behavior

Prove that the embedded receiver can:

- start audio quickly enough to support human validation
- distinguish `wait-config`, `wait-first-audio`, `wait-repair`, and
  `wait-preroll`
- recover if the first audio groups arrive damaged or incomplete

### Priority 3: Rough sync

Prove that audio remains close enough to the visible path for:

- human confirmation
- debug screenshots and logs
- first-song soak validation

### Priority 4: Headroom

Measure whether the selected ESP32 board still has enough margin for:

- lyric-heavy display updates
- Wi-Fi receive bursts
- sustained audio output

## Embedded `quality` policy

`quality` mode should not block the first handheld milestone.

For the first embedded receiver:

- parse and expose `quality` metadata if practical
- allow the receiver to report that `quality` is unsupported or disabled on the
  current build if needed
- do not stall the entire handheld path waiting for acceptable `Opus` decode
  performance

This is consistent with the existing ESP32 audio feasibility gate:

- if local `Opus` decode is too expensive on the first board, keep transport
  compatibility and move local audio to a different path

## Display-first fallback

If local audio fails the embedded feasibility gate, the first receiver must
still be considered useful if it can:

- join sessions reliably
- show useful loading and live visual states
- expose startup and timing diagnostics
- keep the transport and visual path aligned with the desktop sender

That fallback is not a failure of the entire embedded effort. It is an allowed
outcome already anticipated by the existing hardware notes.

## Required wire metadata

The embedded receiver should expect the same concrete metadata direction already
defined by the bad-network audio-profile work:

- `audio_profile_id`
- `codec_id`
- `sample_rate`
- `channels`
- `frame_ms`
- `bitrate_or_mode`
- `join_redundancy_mode`
- `fec_or_repair_mode`
- `startup_preroll_ms`

The embedded receiver must not require out-of-band configuration to initialize
the selected decoder.

## Startup policy

For the first embedded build:

- prefer `resilience` if both profiles are available
- keep startup preroll small enough for useful lab iteration, but not so small
  that normal join becomes unstable
- expose explicit startup state on screen and in logs
- treat missing first audio as recoverable, not terminal

## Output policy

The first embedded audio path should optimize for simplicity:

- mono output is fine
- modest sample rates are acceptable if allowed by the chosen profile
- fidelity is secondary to startup reliability and observability

The first embedded audio path is a validation tool, not the final venue-grade
audio implementation.

## Failure policy

The first embedded build should classify audio outcomes like this:

- `pass`: audio starts reliably and stays acceptably aligned during a full-song
  run
- `conditional`: audio works, but only with reduced settings or visible
  headroom limits
- `fallback`: display path is good, but local audio is disabled or unstable
- `fail`: both display and audio startup remain unreliable

Only the last case should block continued handheld receiver work.

## Practical recommendation

For the first `ESP-IDF` receiver build:

1. implement display and startup observability first
2. target the low-bitrate `resilience` path for local audio
3. keep `quality` optional until measurements justify it
4. preserve a display-first fallback if audio cannot yet be shipped on-board

That keeps the embedded receiver aligned with the repo's transport direction
without forcing the whole project to wait for full embedded `Opus` success.
