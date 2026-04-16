# Embedded Hardware Bring-Up Validation

## Purpose

Validate the first embedded ESP32 receiver hardware path against the goals set by
the hardware and transport notes:

- useful first picture
- usable first audio
- understandable startup state
- survivable weak-link behavior
- enough battery and thermal margin for walk-around testing later

This matrix is intentionally narrower than full product validation. It is meant
to answer one question first:

- is the selected ESP32 display board good enough to justify continued embedded
  receiver work?

## Reference targets

Primary hardware target:

- `Freenove ESP32 CYD 3.2 inch`

Secondary hardware target:

- `Freenove ESP32 CYD 2.8 inch`

Reference software direction:

- `ESP-IDF + FreeRTOS`
- display-first startup path
- low-bitrate embedded audio target first

## Required captured evidence

Each relevant run should capture:

- board revision used
- power source used: `USB` or `battery`
- transport/profile mode used
- first visible screen time
- first useful live visual time
- first audio time if audio is enabled
- startup state transitions
- packet and repair counters
- any visible sync drift notes
- thermal and battery observations when relevant

## Validation stages

### 1. Board bring-up sanity

Purpose:

- prove the selected board is usable for firmware iteration

Checks:

- flash firmware over USB
- reboot cleanly
- serial logs are readable
- display can show a stable boot/status screen
- at least one button input works

Expected result:

- repeatable board bring-up with no mystery boot failures

### 2. First visible screen

Purpose:

- prove the handheld does not feel dead while joining

Checks:

- measure boot-to-first-status-screen time
- measure join-to-loading-screen time
- verify the board can always show a visible state even before full media data is
  ready

Expected result:

- first visible status or loading screen appears quickly and consistently

### 3. First useful live visual

Purpose:

- prove the board can render something meaningful before deep backfill completes

Checks:

- join an in-progress session
- measure time to first useful visual anchor or equivalent live state
- verify the board transitions from loading state to live visual state
- confirm that later backfill or asset work does not blank the screen

Expected result:

- late join gives a useful picture rather than a blank screen

### 4. Startup observability

Purpose:

- make startup failures diagnosable

Checks:

- expose startup states such as `wait-config`, `wait-first-picture`,
  `wait-first-audio`, `wait-repair`, and `wait-preroll`
- verify logs or on-screen counters explain why startup is waiting
- confirm that transport metadata changes are visible

Expected result:

- startup stalls can be triaged without guessing

### 5. First audio

Purpose:

- prove the embedded board can provide a useful local audio confirmation path

Checks:

- enable the first embedded audio path
- measure time from join to first audible output
- verify audio does not wedge permanently if the first groups are incomplete
- confirm the device can complete at least one full-song run without constant
  underrun

Expected result:

- audible startup occurs reliably enough for human validation

### 6. Sync sanity

Purpose:

- verify that audio and visual output are close enough to trust during early
  testing

Checks:

- run a full-song validation on a healthy local link
- note visible lyric/audio alignment by human observation
- capture any obvious drift growth over time
- compare sync behavior during startup versus steady state

Expected result:

- sync remains visibly acceptable for the first handheld proof

### 7. Packet-loss and late-join behavior

Purpose:

- prove the board does not only work on an unrealistically clean link

Checks:

- late join during active playback
- mild reorder or isolated packet loss runs
- startup damage to early audio groups
- verify that the board distinguishes waiting on config, packets, repair, or
  preroll
- verify that video-only success does not permanently wedge audio startup

Expected result:

- the board behaves predictably on small impairments and exposes what went wrong

### 8. Full-song soak

Purpose:

- prove the board can stay alive and useful for more than a quick demo

Checks:

- run one full song on USB power
- capture crashes, resets, freezes, or visible starvation
- capture display update health during lyric-heavy sections
- capture whether audio remains stable enough for human evaluation

Expected result:

- no hard crash or unrecoverable stall in a representative single-song run

### 9. Battery behavior

Purpose:

- confirm that walk-around testing is realistic once USB bring-up is stable

Checks:

- boot and join on battery
- verify no obvious brownout during display or audio peaks
- observe whether battery wiring or charge setup causes resets or noise
- capture rough runtime notes if practical

Expected result:

- battery operation is possible without destabilizing the prototype

### 10. Thermal observations

Purpose:

- detect obvious heat issues before building around them

Checks:

- run the board during combined Wi-Fi, display, and audio load
- note whether the board becomes unusually hot to touch
- note whether heat changes stability, clocking, or screen behavior

Expected result:

- no obvious thermal instability during prototype-length runs

## Pass criteria

The first embedded hardware path should be considered good enough to continue
when it proves:

- repeatable flash and boot
- first visible screen is reliable
- first useful live visual appears consistently
- startup states are visible and understandable
- local audio either works well enough for human validation or fails cleanly into
  a display-first fallback
- late join and light impairments do not create a permanent startup wedge
- one representative full-song run completes on the selected board

## Allowed partial-success outcome

This work is still considered successful if:

- display and transport behavior are solid
- observability is strong
- local audio remains unstable or disabled

In that case, the next step is not to throw away the board. The next step is to
continue as a display-first receiver while audio feasibility is refined.

## Failure criteria

The current board choice should be reconsidered if it cannot reliably achieve
all of these even on USB power and a healthy local network:

- stable boot and flash cycle
- first visible status screen
- useful visual late join
- understandable startup diagnostics

If those basics fail, the issue is no longer just audio feasibility. It means
the board is a poor fit for the first embedded receiver target.
