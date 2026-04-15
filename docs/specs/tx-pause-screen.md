# TX Pause Screen Feature

## Goal

Add a transmitter-side pause mode that:

- toggles from the foreground TX console with `Space` or `p`
- freezes the current track position for both audio and CD+G
- replaces normal live playback output with a repeated custom pause CD+G presentation
- resumes the original track from the exact paused playback position

This is intended for:

- venue intermissions
- repeated RX bring-up during hardware testing
- MCU receiver validation where devices are power-cycled mid-session
- operator-visible "paused but stream still alive" behavior

## Operator behavior

When TX is running in foreground mode:

- `p` toggles pause/play
- `Space` toggles pause/play
- pausing preserves the current playback position
- resuming continues from that exact saved position

When paused:

- TX keeps the current session alive on the network
- TX repeatedly sends a pause CD+G screen instead of advancing the active song
- TX does not advance the track timeline
- TX does not emit normal live song audio frames
- TX still emits `ANNOUNCE`, `CLOCK_BEACON`, PTP traffic, and observability counters

## Pause screen behavior

The pause screen should be a CD+G-authored or CD+G-generated animation layer that can be visually fun:

- static text is acceptable as the first shipped version
- scrolling/motion effects are encouraged
- repeated looping behavior is required
- future variants can include a small library of interchangeable pause-screen themes

The pause screen is not required to be tied to a companion audio asset in the first version.

## Transport behavior

The first implementation should prefer the smallest protocol change that keeps receivers deterministic.

### Required behavior

- TX must advertise paused state through packet flags and/or explicit receiver-observable state
- TX must stop advancing song playout timestamps while paused
- TX must keep the network session alive so RX does not time out or reset unnecessarily
- TX must resume by re-entering the existing late-join/live bootstrap path cleanly

### Preferred first implementation

- keep `DASHCDG_PACKET_FLAG_PAUSED` authoritative for paused state
- treat the pause screen as a TX-side alternate CD+G source
- send pause-screen `CDG_BATCH` and `CDG_SNAPSHOT` packets on a paused timeline separate from the song's advancing media clock
- suppress normal song `AUDIO_FRAME` traffic while paused
- on resume, emit a fresh live `CDG_SNAPSHOT` from the resumed song state and continue normal live media

## Receiver expectations

While TX is paused:

- RX should continue rendering incoming pause-screen CD+G updates
- RX audio playout should become silent once buffered pre-pause audio drains
- RX should not treat pause as transport failure
- RX HUD/headless status should clearly indicate paused state

On resume:

- RX should resume from the preserved song position
- RX should accept a fresh resumed-song snapshot and continue with normal live CD+G batches
- RX should not require a full asset replay before visual recovery

## State model

TX needs explicit paused-session state:

- `paused`
- saved song playback position in ms
- active pause-screen playback position in ms
- pause-screen live CD+G source/state
- whether the current outgoing media source is `song` or `pause-screen`

## Non-goals for the first tranche

- no protocol-level pause-screen asset catalog
- no network-triggered operator control from RX back to TX
- no pause-screen audio or music bed
- no per-receiver pause negotiation
- no theme editor or external authoring workflow beyond a fixed built-in or repo-shipped pause screen

## Open implementation questions

- whether the first pause screen is built-in generated CD+G or loaded from a repo asset
- whether RX should explicitly flush queued audio on pause or simply stop receiving new audio and drain naturally
- whether paused `CLOCK_BEACON.playback_ms` should reflect the frozen song position or the pause-screen loop position

## Recommended first answer

- use a built-in deterministic pause-screen animation first
- freeze announced playback at the saved song position
- keep the pause-screen loop separate from the song playback clock
- drain or clear RX queued audio quickly enough that pause feels immediate, but do not require a new protocol packet to achieve that in the first tranche
