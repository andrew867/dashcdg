# Sample MP3+G media (`cdg/`)

The **`cdg/`** directory at the repository root is a **developer test library** of
paired **`.cdg` + `.mp3`** tracks used for:

- default **TX** / **player** scanning when no path is passed (see root [`README.md`](../../README.md))
- manual soak tests, shuffle behaviour, and “does lyrics still move?” checks across **GL**, **GDI**, and **retro** builds

## Size and shape

A **full** checkout of this folder is on the order of **hundreds of megabytes**
(hundreds of paired tracks). Clones are heavier than a minimal source tree; that is a
deliberate trade-off so CI can zip the same library maintainers use for soak tests.
If your working tree only has a **subset** of pairs (for example after a partial sync),
restore the rest from your canonical backup **before** cutting a release tag so the
published **`dashcdg-sample-tracks-mp3g.zip`** matches expectations.

## GitHub Releases

Tagged releases (for example **`v0.1.0`**) attach:

- **`dashcdg-windows-sneakernet.zip`** — Windows USB bundle (four arch/layout folders)
- **`dashcdg-sample-tracks-mp3g.zip`** — flat archive of everything under **`cdg/`**
  (same pairs as the repo)

Download the sample zip if you want the library **without** cloning the full history.

## Redistribution

These files are **karaoke-style MP3+G pairs** gathered for **engineering validation only**.
If you redistribute them outside a private lab, you are responsible for **licensing and
rights** in your jurisdiction. The project does not grant you a public performance,
sync, or distribution license for the underlying compositions or recordings.

## Adding or replacing tracks

Keep the **same stem** for each pair (`Song.cdg` + `Song.mp3`). Avoid spaces in new
names if you can (Windows + shell quoting are boring). After adding files, update any
local playlists or scripts that depended on specific titles.
