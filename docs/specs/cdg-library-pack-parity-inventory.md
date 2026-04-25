# CDG sample library: PACK parity byte inventory

## Purpose

Many **MP3+G** `.cdg` rips **zero** the six **R–W PACK** parity bytes in every 24-byte subchannel packet after the drive’s error correction, so file bytes alone cannot exercise **Reed–Solomon** parity checks. This note records **which files in the default scan trees actually carry non-zero parity fields**, so tests and manual validation can target them.

Normative background and decoder policy: [`cdg-subchannel-alignment.md`](cdg-subchannel-alignment.md) (PACK **Q** / **P** parity, `dashcdg_cdg_subchannel_pack_rs_syndrome_ok`).

## Definition (matches `dashcdg_subchannel_packet`)

Per packet (**24 bytes**), same layout as `core/include/dashcdg/cdg.h`:

| Byte offset | Field |
| ---: | --- |
| 0–1 | `command`, `instruction` |
| 2–3 | `parity_q[2]` |
| 4–19 | `data[16]` |
| 20–23 | `parity_p[4]` |

A stream **“has PACK parity data in the file”** if **at least one** packet has **any** of bytes **2–3** or **20–23** non-zero.

This is **not** the same as “syndromes verify under our GF(2⁶) RS implementation”; for that, use `dashcdg_cdg_subchannel_pack_rs_syndrome_ok` on each packet with non-zero parity (see tests in `tests/test_core.c`, `test_cdg_subchannel_pack_rs`).

## Scan scope (2026-04-25)

- **`cdg/`** at repository root (recursive `*.cdg`).
- **`docs/specs/`** (recursive `*.cdg`, and `*.zip` members ending in `.cdg`).

**Reproduce:** from repo root,

```bash
python scripts/scan_cdg_pack_parity.py
python scripts/scan_cdg_pack_parity.py --markdown
```

## Results summary

| Metric | Count |
| --- | ---: |
| CDG streams scanned | 214 |
| With ≥1 non-zero parity byte (Q or P) | 17 |
| All parity bytes zero in every packet | 197 |

Of **212** files under **`cdg/`** alone, **15** had non-zero parity in at least one packet; **197** were all-zero parity. Two additional streams live inside **`docs/specs/cdg_examples.zip`**.

## Files with non-zero PACK parity (inventory)

| Path | Packets (24 B) | Packets with any non-zero Q/P | % non-zero |
| --- | ---: | ---: | ---: |
| `cdg/MBFP201-712 - Marshmello & Anne-Marie - Friends.cdg` | 64968 | 19667 | 30.3 |
| `cdg/MBFP201-807 - Paramore - Hard Times.cdg` | 58928 | 14602 | 24.8 |
| `cdg/MBH20182-105 - Ariana Grande - No Tears Leftto Cry.cdg` | 71416 | 19315 | 27.0 |
| `cdg/MBH20182-107 - Benny Blanco ft Halsey & Khalid - Eastside.cdg` | 53008 | 22762 | 42.9 |
| `cdg/MBH20182-110 - Calvin Harris ft Dua Lipa - One Kiss.cdg` | 70212 | 14043 | 20.0 |
| `cdg/MBH20182-210 - Masrshmelloo ft Bastille - Happier.cdg` | 68348 | 17565 | 25.7 |
| `cdg/MBH2019-214 - Marshmello ft Chvrches - Here wit Me.cdg` | 52564 | 14286 | 27.2 |
| `cdg/MBH2022-108 - Ed Sheeran - Shivers.cdg` | 66528 | 22702 | 34.1 |
| `cdg/MBH2022-112 - Harry Styles - As it Was.cdg` | 52752 | 10389 | 19.7 |
| `cdg/MCH20SU-117 - Twenty One Pilots - Level of Concern.cdg` | 70432 | 18036 | 25.6 |
| `cdg/MCH20SU-207 - St John - Roses (Imanbek Remix) (Clean).cdg` | 58640 | 21037 | 35.9 |
| `cdg/MCH22SP-217 - Muse - Won't Stand Down.cdg` | 66404 | 14157 | 21.3 |
| `cdg/MCH23SP-101 - Tiesto ft Tate McRae - 1035.cdg` | 57252 | 15653 | 27.3 |
| `cdg/SFD7014-12 - Seger, Bob - Hollywood Nights.cdg` | 96308 | 14437 | 15.0 |
| `cdg/VSHPARTY-109 - David Bowie - Let'sDance.cdg` | 79632 | 15203 | 19.1 |
| `docs/specs/cdg_examples.zip::plexcdg.cdg` | 59420 | 58154 | 97.9 |
| `docs/specs/cdg_examples.zip::toshcdg.cdg` | 55008 | 54819 | 99.7 |

## Archives under `docs/specs/` without embedded `.cdg`

These zips are **source / player** bundles only (no `.cdg` members): `cdglib01.zip`, `cdglib02.zip`, `cdglib03.zip`, `cdgply12.zip`.

## Related

- Sample library policy: [`docs/test/sample-media.md`](../test/sample-media.md)
- TX v4 audio starve + `v4-rx-peer` health RCA: [`desktop-tx-v4-audio-starve-rx-health-rca.md`](desktop-tx-v4-audio-starve-rx-health-rca.md)
