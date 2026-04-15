# Pause Screen Validation

## Purpose

Validate TX pause/play behavior for both desktop RX and future MCU-style receiver testing.

## Manual proof matrix

### Baseline pause/resume

1. Start TX on a normal `MP3+G` track.
2. Start RX in GUI mode and confirm normal audio/video playback.
3. Press `p` on TX.
4. Confirm:
   - TX reports paused state in stdout/HUD
   - RX keeps receiving packets
   - RX shows the pause screen quickly
   - RX audio becomes silent
5. Press `p` again.
6. Confirm:
   - original song resumes from the paused position
   - RX returns from pause screen to live song rendering
   - no full reboot/reset is needed

### Space bar toggle

1. Repeat the same test using `Space` instead of `p`.
2. Confirm it behaves identically.

### Late join while paused

1. Start TX and let the song begin.
2. Pause TX.
3. Start RX after pause is already active.
4. Confirm:
   - RX shows the pause screen without waiting for full asset bootstrap
   - RX does not falsely report transport stall
   - RX status shows paused state

### Resume after late join

1. With the late-joined RX still connected during pause, resume TX.
2. Confirm:
   - RX transitions back to the song
   - resume position is close to the frozen song position, not to song start
   - live `CDG_BATCH` apply counters advance again

### Repeated pause cycles

1. Pause and resume 10 times on the same track.
2. Confirm:
   - no TX deadlock
   - no RX freeze/not-responding state
   - no steady counter explosion from skip/drop logic
   - no unrecoverable audio queue corruption

### Receiver power-cycle simulation

1. Start TX and pause it on the pause screen.
2. Start and stop RX repeatedly during the pause interval.
3. Confirm each new RX instance:
   - discovers the session
   - renders the pause screen
   - stays responsive in GUI and headless modes

## Headless signals to watch

TX:

- paused flag/state visible in status output
- song playback position remains frozen while paused
- normal song audio frame counters stop advancing during pause
- pause-screen CD+G counters continue advancing

RX:

- paused state visible in status output
- `render=` should remain active rather than falling back to `wait-announce`
- `live_applied` should continue for pause-screen updates
- audio buffered ms should drain and stabilize without runaway decode failures

## Regression checks

- normal play still works when pause is never used
- current late-join snapshot behavior still works for non-paused sessions
- FEC and PTP counters remain sane during pause and resume
- track end/auto-next still works after resuming from pause

## Suggested automated coverage

Unit or focused logic tests should cover:

- pause toggle preserves and restores playback anchor position
- pause mode suppresses normal song audio scheduling
- resume re-enables normal song audio scheduling
- pause mode switches live CD+G source from song to pause-screen and back
- paused state remains visible in announced/flagged transport metadata

## Current completion criteria

This feature is considered proven when:

- GUI RX survives repeated pause/resume cycles without freezing
- headless RX shows paused state and resumed live counters correctly
- late join during pause reaches the pause screen quickly
- resume returns to the original song rather than restarting or drifting far
