# Quality Gates and Release Criteria

## Tranche 0-1

- build portable core and tests successfully
- CD+G semantics covered for memory, border, tile, scroll, transparency, seek
- documentation exists for baseline seams and portable core boundaries

## Tranche 2-3

- protocol serialization and parsing covered by tests
- desktop TX/RX proof runnable from the repo
- documented impairment matrix for loss, jitter, and late join
- packet captures can be analyzed via sequence and sender timestamps

## Tranche 4-6

- ESP-IDF platform assumptions documented
- hardware bring-up and production validation plans written before board spin
- explicit audio feasibility gate written before committing to onboard audio

## CI expectations

The repository should eventually enforce:

- compile and test on every merge request
- separate jobs for core tests and desktop app compilation
- artifact retention for test binaries and captured logs
- no release tag without passing automated tests and updated docs
