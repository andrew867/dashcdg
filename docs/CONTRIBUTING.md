# Contributing to dashcdg

Thanks for being curious enough to open this file. The project lives at the
intersection of “real-time-ish UDP”, CD+G trivia, and Windows builds that need
to run on a USB stick. Here is how to help without getting lost in the
scaffolding.

## What to open first

| You want to… | Start here |
| --- | --- |
| Understand the **desktop** story (builds, zips, sneakernet) | [`specs/desktop-platform-support.md`](specs/desktop-platform-support.md) |
| Map **modules** and libraries | [`architecture/transport-and-playout-modules.md`](architecture/transport-and-playout-modules.md) |
| **Receiver** invariants and debugging | [`../AGENTS.md`](../AGENTS.md) and [`specs/receiver-progress-invariants.md`](specs/receiver-progress-invariants.md) |
| **Wire protocol** | [`specs/transport-protocol.md`](specs/transport-protocol.md) |
| “Why is this repo so **extra**?” | [`fork-manifesto.md`](fork-manifesto.md) |

## Build and test (the short version)

```sh
make
make test
```

`make test` links and runs the core/protocol/desktop-adjacent tests. On **Windows
MSYS2**, the desktop stack also needs **vendored libsoxr** for high-quality
resampling before you link the full player:

```sh
make vendor-soxr
make desktop-apps   # or your usual make debug / dist target
```

Details: [`specs/windows-legacy-mingw-build.md`](specs/windows-legacy-mingw-build.md),
[`specs/pcm-libsoxr-desktop-src.md`](specs/pcm-libsoxr-desktop-src.md).

## Commits and branches

- Prefer **one logical change** per commit (easier to bisect when multicast does
  something cursed on a Friday night).
- A clear **title** and a **body** that explain *what* and *why* beat a novel’s
  worth of `fix stuff`.
- **Do not commit** `build/` artifacts, local logs, or giant binaries unless
  the project explicitly tracks them (it usually does not).

## Documentation

- **Index of all docs:** [`docs/README.md`](README.md)
- If you add a new **normative** spec, link it from `docs/README.md` so the next
  person can find it.
- Files named `*-rca.md`, `*-brief.md`, or `*-implementation-plan.md` are often
  **point-in-time** postmortems or plans. Treat them as history unless they are
  linked as current architecture.

## Code style (practical)

- **C99** in the hot path; match surrounding style in each file.
- Run the same `make` / `make test` (and desktop builds if you touched
  `platform/desktop/`) before you push, especially on Windows if you can.

## License

The tree ships under the [MIT `LICENSE`](../LICENSE) at the repository root
(see file for copyright line). Contributions should be compatible with that
license.
