# V4 Rollout Validation Report

## Scope

This report captures the concrete proof gathered during the first bad-network v4
rollout implementation on the Windows desktop proof host.

It complements, but does not replace, the matrices in:

- `docs/test/bad-network-transport-validation.md`
- `docs/test/portability-streaming-validation.md`

## Commands Run

Windows build and packaging:

- `mingw32-make debug`
- `./build/bin/test-core`
- `mingw32-make package`

Local v4 loopback smoke:

- `./build/bin/desktop-rx --headless 127.0.0.1 34567`
- `./build/bin/desktop-tx --badnet-v4 127.0.0.1 34567 smoke "./cdg" 100`

Throughput-clamped relay smoke:

- `python scripts/desktop_impairment.py --listen-group 239.255.77.91 --listen-port 24684 --emit-group 239.255.77.92 --emit-port 24685 --max-bytes-per-second 112500 --max-packets 800`
- `./build/bin/desktop-rx --headless 239.255.77.92 24685`
- `./build/bin/desktop-tx --badnet-v4 --audio-profile=resilience 239.255.77.91 24684 clamp "./cdg" 100`

## Observed Results

### Windows baseline

- `mingw32-make debug` completed successfully.
- `./build/bin/test-core` reported `all tests passed`.
- `mingw32-make package` produced `build/release/dashcdg-windows-portable.zip`.

### Local v4 loopback

- TX emitted structured v4 startup events for `loading_screen`, `first_anchor`,
  `first_audio`, and `running`.
- RX parsed v4 packets with `proto=4`, `prof=1`, and `codec=1`.
- RX counters advanced for `SESSION_INFO`, `LOADING_SCREEN`, `VIDEO_ANCHOR`,
  `AUDIO_CHUNK`, `VIDEO_DELTA`, `BACKFILL_CHUNK`, and `CLOCK_SYNC`.
- RX reached `gate=running` and `render=live-snapshot` before full asset rebuild
  completed, showing the intended startup path.
- Backfill progressed while live audio and video continued, proving the split
  between startup visuals and full asset replay.

### Throughput-clamped relay

- The relay forwarded exactly `800` packets and reported `429996` bytes out under
  the configured `112500 B/s` cap.
- The `resilience` profile reached RX with `proto=4`, `prof=2`, and `codec=2`.
- RX remained alive under the capped relay after the resilience playout-rate fix.
- RX continued applying live visuals while backfill advanced to `329/1733`
  chunks.
- Startup audio still showed `decode_fail=48` during this first constrained
  resilience proof, so the profile now survives the run but still needs more
  tuning before it should be called clean.

## Matrix Status

Bad-network matrix:

- Healthy baseline: partially proven on local loopback for `quality`.
- Throughput pressure: partially proven on the capped relay for `resilience`.
- Mixed burst loss plus reorder: not run in this report.
- Late join with delayed anchor: not run in this report.
- Late join with damaged first audio groups: not run in this report.
- Long soak under mild impairment: not run in this report.

Portability and slimdown matrix:

- Current live-wire baseline: proven on Windows loopback.
- Stage A memory reduction proof: previously proven by status output and build/test,
  not re-measured in this report.
- Stage B source abstraction proof: proven by current `src=file` TX status on the
  headless path and successful v4 loopback smoke.
- Preview-path compatibility: not re-run in this report.
- Windows baseline smoke: proven by build, tests, and package creation.
- Linux baseline smoke: not run on this host.
- macOS baseline smoke: not run on this host.
- Legacy Windows research gate: remains documentation-only research.

## Current Conclusion

The v4 rollout is implemented and buildable on the Windows proof path, with a
working local startup path and a working capped-relay proof harness.

The strongest remaining validation gap is not code generation but proof depth:

- resilience startup still needs cleanup because the constrained run logged early
  audio decode failures
- the loss, reorder, late-join, and long-soak matrix rows still need dedicated
  runs and captured logs
- Linux and macOS portability claims remain documented targets rather than
  host-verified results in this report
