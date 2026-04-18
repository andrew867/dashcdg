# TX CD+G source model — late-join regression plan

## Purpose

Regression checklist for **[tx-cdg-source-model.md](../specs/tx-cdg-source-model.md)** refactor stages (**memory slimdown**, random-access source, preview fallback). Each stage must preserve **wire semantics** listed in that spec §Current Guarantees To Preserve.

## Scope

- Desktop TX variants: headless, GL preview, GDI preview where applicable.
- Desktop RX: at least **one** modern + **one** legacy path (e.g. GDI + WinMM) if Windows-specific timing differs.

## Case matrix

Run **before** and **after** each merge that touches `cdg_source`, `asset_bytes`, `cdg_batches`, snapshot buffers, or asset replay scheduling.

| ID | Scenario | Steps | Pass criteria |
| --- | --- | --- | --- |
| **LJ-1** | Cold RX after TX started | Start TX + play ≥ 30 s; start RX | RX completes bootstrap; **first audio** within announced preroll band; graphics non-blank after snapshot/live merge |
| **LJ-2** | Late join mid-track | RX starts ≥ 60 s after TX | Same as LJ-1; **no** permanent blank raster; clock_sync acceptable |
| **LJ-3** | Jump TX track while RX joining | Trigger next track during LJ-2 | RX recovers without manual restart; gates eventually **running** |
| **LJ-4** | Pause / resume TX | Pause ≥ 10 s; resume | RX shows pause screen then live; audio resumes |
| **LJ-5** | Asset replay completeness | RX-only capture: compare reconstructed asset size/hash to TX source file | Deterministic match per protocol |
| **LJ-6** | FEC path | Optional: mild impairment relay during LJ-1 | Repair counters plausible; no infinite stall |

## Stage-specific gates (from tx-cdg-source-model)

When implementing **Stage B+** (random-access source, reduced duplication):

- **Preview mode** whole-memory fallback: run **LJ-1–LJ-4** with preview **on** and **off**.
- **Wire path** default: **LJ-1–LJ-5** must pass without preview-only code paths.

## Evidence

For each release candidate:

1. Table of case IDs × pass/fail × commit.
2. Optional: attach RX screenshot or HUD line proving `gate=running`.

## Related

- **[desktop-proof-plan.md](desktop-proof-plan.md)** — broader desktop claims.
- **[bad-network-transport-validation.md](bad-network-transport-validation.md)** — if testing under impairment concurrently.
