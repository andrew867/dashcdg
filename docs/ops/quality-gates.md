# Quality Gates and Release Criteria

## Tranche 0-1

- build portable core and tests successfully
- CD+G semantics covered for memory, border, tile, scroll, transparency, seek
- documentation exists for baseline seams and portable core boundaries

## Tranche 2-3

- protocol serialization and parsing covered by tests
- desktop TX/RX proof runnable from the repo over default multicast and explicit broadcast endpoints
- live `AUDIO_FRAME` and `CDG_BATCH` transport observable in TX/RX status output
- live `CDG_SNAPSHOT` receipt/apply observable in TX/RX status output
- bounded `FEC_PARITY` generation and recovery observable in TX/RX status output
- documented impairment matrix for loss, jitter, burst loss, and late join
- repeatable desktop impairment relay workflow available from the repo
- packet captures can be analyzed via sequence and sender timestamps
- developer-facing docs explain the current TX/RX architecture, protocol families, proof claims, and known gaps without stale `v2`/local-MP3 wording

## Tranche 4-6

- ESP-IDF platform assumptions documented
- hardware bring-up and production validation plans written before board spin
- explicit network-audio feasibility gate written before committing to onboard audio
- protocol docs must call out implemented vs reserved packet families so proof behavior is not overstated
- desktop proof docs must clearly separate "implemented", "proven", and "planned" so new developers do not mistake design intent for completed work

## CI expectations

The repository should eventually enforce:

- compile and test on every merge request
- separate jobs for core tests and desktop app compilation
- desktop smoke validation logs for TX/RX proof changes
- doc checks or review gates for protocol/architecture changes that alter operator-visible behavior
- artifact retention for test binaries and captured logs
- no release tag without passing automated tests and updated docs
