# CD+G subchannel byte alignment (TX)

## Purpose

Some `.cdg` assets begin with a non-zero byte offset relative to true **24-byte
subchannel packets**, or end with a short byte remainder. Feeding those bytes
to the packet reader as if byte 0 were a `command` field causes sparse
`0x09` matches and wrong graphics timelines.

This document records how **dashcdg** infers framing **without** the unsafe
heuristic of “count raw `0x09` bytes every 24 bytes”, which can score higher on
misaligned windows than on the real boundary.

## Wire layout (Red Book / CD+G mode 5)

Each subchannel packet is **24 bytes** (see `struct dashcdg_subchannel_packet` in
`core/include/dashcdg/cdg.h`):

| Offset | Field        | Notes                                              |
|--------|--------------|----------------------------------------------------|
| 0      | `command`    | Only bits 0–5 used; CD+G uses **0x09** (mode 5).   |
| 1      | `instruction`| Only bits 0–5; defines the graphics opcode.        |
| 2–3    | `parity_q`   | R–W subchannel **Q** parity (see below).         |
| 4–19   | `data[16]`   | Instruction payload; only low 4–6 bits used.     |
| 20–23  | `parity_p`   | R–W subchannel **P** parity (see below).         |

Public references that match this layout and the instruction set include
[CD+G Revealed](https://jbum.com/cdg_revealed.html) (informal, widely cited) and
Philips / IEC **Red Book** family documents (licensed “Green Book” CD+G
extension). Official parity generator details are not duplicated here pending a
citable excerpt in-repo.

## Normative PDFs in `docs/specs` (reviewed)

These files were checked with `pdftotext` (text extraction) to see whether we can
implement **R–W PACK P/Q parity** checks from copy-pastable standard text.

| File | Role | Extractable text? | Use for CD+G parity |
|------|------|-------------------|-------------------|
| `IEC_60908_1999_International_Standard.pdf` | IEC “Red Book” audio / subcode | **Almost none** (~18 KiB of non-body boilerplate only) | **Yes, normatively** — clauses **17** (subcode), **19** (PACK, **19.3–19.9** P- and Q-parity encoders/decoders, interleave), **17.6** (R–W), **22** TV-GRAPHICS — but the edition in-repo appears **image-based**; equations live in figures **21–24** (P/Q encoder/decoder). |
| `BS_EN_60908_1999_British_Standard.pdf` | National adoption of 60908 | **Almost none** | Same content target as IEC 60908; same OCR/searchability issue. |
| `info_iec60908b.pdf` | IEC 60908 second edition (1999) | **Yes** (TOC, foreword, figure list) | Confirms **clause and figure pointers** (e.g. **19.7–19.9**, figures **23–24** Q-parity encoder/decoder) but does **not** include the full technical body in this extract. |
| `is.iec.60958.3.2003.pdf` | **IEC 60958-3** (consumer serial digital audio / AES3-style interface) | **Yes** (~80 KiB+) | **Not** the definition of subchannel **PACK** Reed–Solomon. Annex material describes how **CD-derived** streams map **control / user (U) bits** to the interface (e.g. subcode block length, Q bits) and points to **IEC 60908** for the disc format. Useful for **S/PDIF + CD**, not for verifying the six parity bytes inside a 24-byte R–W graphics symbol. |

**Conclusion:** the answers for **implementable P/Q syndrome checks** on raw
24-byte PACKs are in **IEC 60908** (same technical territory as Philips Red Book
subcoding), **§19** and related figures — not in IEC 60958-3. The 60908 PDFs
currently in-tree need a **text layer or OCR** (or a different electronic edition)
before we can transcribe matrices safely into code and tests.

### Related: [carrotIndustries/redbook](https://github.com/carrotIndustries/redbook)

That project walks through **channel-bit** decoding (EFM, framing, subcode
assembly) and includes a working **Q-channel subcode block CRC** in
[`decode.py`](https://github.com/carrotIndustries/redbook/blob/master/decode.py):
polynomial **x¹⁶ + x¹² + x⁵ + 1** (`0x1021`, CRC-CCITT style), with the **last 16
Q bits taken inverted** before the remainder is checked for zero (see the
article README on the same repo).

That CRC applies to the **96-bit Q payload inside a 98-frame subcode block**
(timing / TOC style Mode 1 Q data). It is **not** the same mechanism as **P- and
Q-parity Reed–Solomon symbols inside a 24-byte R–W graphics PACK** (the
`parity_q[2]` / `parity_p[4]` fields in `dashcdg_subchannel_packet`). PACK parity
is still defined only from **IEC 60908 §19** (or an equivalent searchable
excerpt). The redbook repo is still a useful sanity reference for how subcode
bits are gathered from frames before any MODE=5 graphics interpretation.

## Parity bytes in `.cdg` files

On disc, Q and P protect the subchannel codeword. Many software rips **zero**
the six parity bytes after the drive’s error correction, so a decoder cannot
reliably reconstruct syndromes from file bytes alone.

### MIT implementation (`core/src/cdg_subchannel_rs.c`)

`dashcdg_cdg_subchannel_pack_rs_syndrome_ok` and `dashcdg_cdg_subchannel_pack_rs_fill`
implement GF(2⁶) Q- then P-parity over the 24-byte PACK (6-bit symbols). Golden
bytes and round-trip cases live in `tests/test_core.c` (`test_cdg_subchannel_pack_rs`).

**Current policy** (`dashcdg_cdg_compute_subchannel_trims`)

- If all six parity bytes are **zero**, RS is not required for that packet (typical rip).
- If **any** parity byte is non-zero, the packet counts toward alignment scores only
  when `dashcdg_cdg_subchannel_pack_rs_syndrome_ok` is true, so random misaligned
  bytes are less likely to fake a long run of “valid” graphics headers.

## Alignment scoring (implementation)

`dashcdg_cdg_compute_subchannel_trims` in `core/src/cdg.c`:

1. For each candidate start offset **0…23** over the first **N** packets (same
   `N` for every offset so high offsets are not short-changed by tail length),
   parse 24-byte structs.
2. Require `(command & 0x3F) == 0x09` and a known CD+G `instruction`. If any
   parity byte is non-zero, require `dashcdg_cdg_subchannel_pack_rs_syndrome_ok`
   (PACK RS); all-zero parity skips RS for that packet.
3. Count a **header hit** for each packet that passes step 2.
4. Count a **field hit** when, in addition, instruction-specific low-bit fields
   look plausible (e.g. tile row/column inside the 300×216 grid; scroll command
   subfields in 0…2).
5. Tie-break with header hits, then with “parity bytes all zero” among
   header-valid packets (weak signal for rips).
6. Choose a non-zero **prefix trim** only if the best offset beats the runner-up
   by a margin **and** beats offset 0 by enough field-hits to avoid ambiguous
   picks.

**Suffix trim** is always `(total_bytes - trim_prefix) % 24`: orphan tail bytes
cannot be a full subchannel packet.

## TX integration

`dashcdg_tx_load_track_locked` in `platform/desktop/src/app_tx.c`:

- **Preview / in-memory path**: trim after full read, before the CDG reader and
  `cdg_source_open_memory`.
- **Headless file-backed path**: peek the start of the file (same cap as the
  core scan window). If either trim is non-zero, load the whole file into
  memory, apply trims, and use a memory-backed source (misaligned rips are rare;
  this keeps behavior correct without extending the file source API).

## Non-goals

- Replacing full **CIRC / RS** decoding for audio sectors.
- Using dashcdg **transport FEC** parity (`dashcdg/fec.h`) as CD subchannel
  parity (different layer).
