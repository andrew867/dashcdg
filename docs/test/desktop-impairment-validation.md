# Desktop Impairment Validation

## Purpose

This document defines a repeatable desktop proof workflow for loss, reorder, and burst-loss validation using the multicast impairment relay in `scripts/desktop_impairment.py`.

For simple direct TX/RX bring-up, the desktop apps now default to `239.255.77.77:24684` and can also run against explicit IPv4 broadcast endpoints. This document intentionally uses separate multicast input/output groups so the relay can isolate clean input traffic from impaired output traffic.

## Topology

Use two multicast groups:

1. TX sends clean media to the relay input group.
2. The relay joins that input group, applies impairments, and forwards to a second output group.
3. RX listens only on the relay output group.

The relay itself is multicast-specific today; broadcast support applies to direct desktop TX/RX runs, not to the impairment relay CLI.

Example groups:

- relay input: `239.255.77.91:24684`
- relay output: `239.255.77.92:24685`

## Commands

Start RX:

```sh
build/bin/desktop-rx --headless 239.255.77.92 24685
```

Start the impairment relay:

```sh
python scripts/desktop_impairment.py \
  --listen-group 239.255.77.91 \
  --listen-port 24684 \
  --emit-group 239.255.77.92 \
  --emit-port 24685
```

Start TX:

```sh
build/bin/desktop-tx 239.255.77.91 24684 "C:/Users/andrew/OneDrive - Green O365/Documents/GitHub/dashcdg/cdg" 1000
```

## Suggested matrix

### Baseline relay

Command:

```sh
python scripts/desktop_impairment.py \
  --listen-group 239.255.77.91 \
  --listen-port 24684 \
  --emit-group 239.255.77.92 \
  --emit-port 24685
```

Expected:

- RX `gate=` should move through `wait-ptp`, `wait-preroll`, and into running state.
- RX `repair` counters should stay near zero.
- TX `ovh=` and `prof=` should be visible.

### Periodic single-packet loss

Command:

```sh
python scripts/desktop_impairment.py \
  --listen-group 239.255.77.91 \
  --listen-port 24684 \
  --emit-group 239.255.77.92 \
  --emit-port 24685 \
  --drop-every 11
```

Expected:

- RX `repair aud=` and/or `repair live=` should increase.
- RX `fail=` should remain materially lower than the total dropped count when the dropped packet falls inside a fully covered FEC group.

### Reordering

Command:

```sh
python scripts/desktop_impairment.py \
  --listen-group 239.255.77.91 \
  --listen-port 24684 \
  --emit-group 239.255.77.92 \
  --emit-port 24685 \
  --reorder-every 9 \
  --reorder-hold-ms 80
```

Expected:

- RX `reord=` counters should increase.
- RX should continue to make progress without permanently wedging on one missing sequence.

### Burst loss

Command:

```sh
python scripts/desktop_impairment.py \
  --listen-group 239.255.77.91 \
  --listen-port 24684 \
  --emit-group 239.255.77.92 \
  --emit-port 24685 \
  --burst-every 25 \
  --burst-length 3
```

Expected:

- RX `fail=` should increase more than in the periodic single-loss case.
- RX `repair hot=` may fall when more than one member in a group is missing.
- This case demonstrates the current bounded XOR limit rather than guaranteed recovery.

### Mixed impairment

Command:

```sh
python scripts/desktop_impairment.py \
  --listen-group 239.255.77.91 \
  --listen-port 24684 \
  --emit-group 239.255.77.92 \
  --emit-port 24685 \
  --drop-every 17 \
  --reorder-every 13 \
  --burst-every 41 \
  --burst-length 2 \
  --seed 42
```

Expected:

- RX should remain observable and should continue reporting gate, sync, and repair state.
- Use this case for manual soak runs and log capture.

## Log fields to watch

RX:

- `gate=`: startup state and playout readiness
- `render=`: bootstrap asset readiness
- `repair aud=` / `repair live=` / `fail=`: repair success and failure totals
- `grp=` / `parity=` / `hot=`: current FEC working set and immediately repairable groups
- `sync off=` / `path=` / `step=` / `peak=` / `hold=`: clock quality and holdover behavior

TX:

- `fec=`: parity packets sent
- `ovh=`: current FEC packet overhead percentage
- `prof=`: active audio/CD+G group profile
- `lead aud=` / `lead live=`: current send lead relative to playback

Relay:

- `stats:` lines show received, forwarded, dropped, reordered, and burst-dropped packet totals

## Current interpretation

- Passing baseline plus periodic single-loss runs demonstrates the intended proof target for the current bounded XOR FEC design.
- Burst-loss runs are expected to show limits, because the current desktop proof repairs only one missing media payload per protected group.
