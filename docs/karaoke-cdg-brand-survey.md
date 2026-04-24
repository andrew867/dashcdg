# MP3+G / CD+G authoring survey (in-repo rips)

This document records **quantitative and structural** observations from **CD+G files present under `cdg/`** in the dashcdg repository, plus **engineering implications** for `dashcdg` TX/RX (seek, keyframes, v4 anchors, tile throughput). It is **not** a legal or musicological history of when masters were produced; bytes alone cannot prove disc age.

---

## 1. How packets were measured

- Each **CD+G subchannel pack** is treated as **24 bytes** (see `struct dashcdg_subchannel_packet` in `core/include/dashcdg/cdg.h`).
- **`command & 0x3F == 0x09`** is the **TV-GRAPHICS** channel for CD+G (`docs/specs/cdg-subchannel-alignment.md`). Other command values still advance the logical timeline in `dashcdg_cdg_state_process_packet()` but do not execute graphics opcodes.
- **Instruction** values use the `DASHCDG_INSN_*` enum names in prose: **MEM** (1), **BORDER** (2), **TILE** (6), **SCROLL_PRESET** (20), **SCROLL_COPY** (24), **DEF_TRANSPARENT** (28), **CLUT0/CLUT8** (30/31), **TILE_XOR** (38).
- **Leading idle**: count of consecutive packs from the start of the file with **`command == 0`** (and typically all-zero payload in these rips).
- **First MEM₀**: first **`MEMORY_PRESET`** with **`repeat & 0x0F == 0`** (first full framebuffer clear in this decoder).
- **Z-style open (8–15)**: first eight graphics packs are **MEM** with the **same color** and **`repeat` running 8, 9, …, 15** — advances time **without** clearing (see `core/src/cdg.c` `MEMORY_PRESET` branch).
- **Trailing zeros**: consecutive **all-zero 24-byte** packs at **EOF** (padding / timeline slack vs paired MP3).
- **cmd0%**: percentage of all packs in the file with **`command == 0`**.

---

## 2. Label glossary (bracket codes on files)

| Code | Meaning (as provided or commonly used) | Notes |
|------|------------------------------------------|--------|
| **KM** | KaraokeMaker.com / “Made in Canada” style house | User-supplied mapping. |
| **Z** | Zoom Karaoke | User-supplied mapping. |
| **DK** | DK Karaoke | User-supplied mapping. |
| **SF** | Sunfly | User-supplied mapping. |
| **SC** | Sound Choice | User-supplied mapping. |
| **L** | Legends Series | User-supplied mapping. |
| **NU** | Nutech | User-supplied mapping. |
| **DC** | Dreamcatcher Karaoke | User-supplied mapping (no sample in this pass). |
| **P** | Pioneer (“The Art of Entertainment” era discs, e.g. LDCA) | **Chicago – 25 Or 6 To 4 [P]** sampled in-repo. |
| **AS** | All Hits–style tag on some files (e.g. Foster **Don’t Stop [AS]**) | May overlap **AH** product family; not legally verified here. |
| **SGB** | Sound Gallery / SGB-style tag on rip | Observed on **Jeff Healy** file in-repo. |
| **MBFP** | Pop / chart prefix (e.g. MBFP201-807) | Catalog-style **Music Bay / chart pack** naming pattern; treat as **product line**, not decoded here as a single company. |
| **MBH** | Catalog prefix (e.g. MBH2022-108, MBH20182-…) | **Year in code** suggests periodic chart SKU; **MBH20182-*** context: **Mr Entertainer** (mrentertainer.co.uk) per user. |
| **MBH20182** | Mr Entertainer chart SKU line (2018×2 series) | **Ariana – No Tears…** sample: **CLUT → BORDER → MEM** with **non-contiguous repeat ladder** (see §6.16). |
| **ZPCP** | Zoom catalog / product code (**ZPCP2023-…** = 2023 Zoom line) | **Die for You** sample matches **~300-pack idle + MEM₀** “modern block” (§6.15). |
| **VSHPARTY** | **Vocal Hits** / party-series catalog prefix (**VSHPARTY-…**) | Vendor decode from filename; **Bowie Let’s Dance** sample (§6.14). |
| **MM** | **Music Maestro** | User-supplied mapping. |
| **SN** | **Sing It Now** (powered by / tied to **Pop Hits Monthly**) | User-supplied mapping. |
| **KB** | **KaraokeBay.com** | User-supplied mapping. |
| **MH** | **Monster Hits** | User-supplied mapping. |
| **PHM** | Pop Hits Monthly | User-supplied mapping. |
| **MF** | Music Factory | User-supplied mapping. |
| **KV** | Karaoke Version | User-supplied mapping. |
| **ME / MR** | Mr Entertainer | User-supplied mapping. |
| **BH** | “BH Karaoke” tag on rip | Vendor identity not independently verified. |
| **CB** | **Chartbuster** (karaoke industry convention) | Widely mapped; **Weezer [CB]** metrics match **tile-heavy classic** Chartbuster-era style. |
| **AH** | **All Hits Karaoke** | **Not one encoder**: **Pink Floyd – Time** is **scroll+CLUT** heavy; **STP – Interstate Love Song** is **XOR-only, no scroll** (§6.8, §6.21). |

---

## 3. High-level pattern taxonomy

1. **Long idle → graphics** — Hundreds of **`command == 0`** packs, then **`0x09`**. Stresses **reader seek** (must advance on timeline even when `process_packet` returns 0). *Examples: Cyndi DK (324), Tom Petty SF (131), Foster AS (299), Black Velvet DK (324), Pink Floyd AH (299).*
2. **Z-open (MEM repeat 8–15) → idle → MEM₀** — Graphics start immediately but **no clear** until later **`repeat == 0`**. Same seek stress as (1) but wire shows **`0x09` from byte 0**. *Examples: Eagles Z, ELO Evil Woman Z, Jackson Browne L (hybrid), MBH Shivers (partial ladder tail), Layla MF (partial ladder tail).*
3. **KM / “modern still” intro** — Short idle (optional), **CLUT → BORDER → MEM color 15 ladder →** repeated **CLUT/BORDER/MEM** blocks, then **TILE + XOR**. *Examples: Toronto KM, Tragically Hip KM.*
4. **XOR-only picture** — After init, **only `TILE_XOR`** (no plain **`TILE`**). *Example: Vance Joy BH.*
5. **XOR-heavy + some TILE** — Modern full-frame stills. *Examples: Healy SGB, Paramore MBFP, Foster AS, Helena KV, FOB PHM, ELO Z.*
6. **Tile-heavy classic** — Large **`TILE`** count vs XOR; often **more CLUT churn** across the song. *Examples: Black Velvet DK, Cyndi DK, America NU, Weezer CB (extreme TILE bias).*
7. **Scroll-driven motion** — Nonzero **`SCROLL_*`** usage. **Rare** in the MP3+G rips surveyed here. **Pink Floyd – Time [AH]** uses scroll **throughout**. **Weezer – Hash Pipe [PHM]** packs **~296** scrolls almost entirely in an **EOF lump** (credits/outro-style), not an intro wipe. Diagonal “wipes” on other files were shown to be **tile placement + XOR**, not scroll.

---

## 4. Master metrics table (representative on-disk files)

Values are **whole-file** counts unless noted. **C0/C8** = `LOAD_COLOR_TABLE_00/08` instruction counts. **SCR** = `SCROLL_PRESET + SCROLL_COPY`.

| Example file (in `cdg/`) | Tag | Lead idle | First MEM₀ pack | TILE | XOR | MEM | C0/C8 | SCR | DEF | cmd0% | Trail ∅ packs | Notes |
|--------------------------|-----|------------:|------------------:|-----:|----:|----:|------:|----:|----:|------:|----------------:|-------|
| Cyndi Lauper – Time After Time | DK | 324 | 324 | 6334 | 33046 | 176 | 153/153 | 0 | 0 | 42.2 | 2946 | DK long-idle reference |
| Alannah Myles – Black Velvet | DK | 324 | 324 | 6763 | 32745 | 208 | 127/127 | 0 | 0 | 54.0 | 5964 | Palette-heavy “classic” feel |
| Tom Petty – Learning To Fly | SF | 131 | 133 | **0** | 8562 | 432 | 1/1 | 0 | 0 | 84.6 | 2525 | XOR-only body after intro; no plain TILE |
| Eagles – Peaceful Easy Feeling | Z | 0 | 308 | 1529 | 18715 | 272 | 117/117 | 0 | 58 | 73.4 | 0 | Z-open + idle; high DEF |
| Electric Light Orchestra – Evil Woman | Z | 0 | 308 | 1395 | 19021 | 284 | 101/101 | 0 | 50 | 73.9 | 0 | Same Z-open fingerprint as Eagles |
| Jackson Browne – Take It Easy | L | 0 | 308 | 596 | 16819 | 112 | 12/12 | 0 | 2 | 75.3 | 4 | Z-open + idle + AS-like second block |
| Derek & The Dominos – Layla | MF | 0 | 311 | 296 | 6715 | 336 | 4/4 | 0 | 1 | 85.4 | 0 | Z-**tail** (r11–15) then long idle |
| MBH2022-108 – Ed Sheeran – Shivers | MBH | 0 | 307 | 618 | 21976 | 95 | 3/3 | 0 | 1 | 65.9 | 0 | Z-**tail** (r13–15) then long idle |
| MBFP201-807 – Paramore – Hard Times | MBFP | 314 | 11 | 586 | 13887 | 113 | 4/4 | 0 | 1 | 75.2 | 0 | KM-style ladders; column striping |
| Toronto – Your Daddy Don’t Know | KM | 16 | 11 | 6184 | 9291 | 128 | 8/8 | 0 | 0 | 74.0 | 2267 | Mixed TILE/XOR |
| Tragically Hip – Locked In The Trunk… | KM | 8 | 11 | 10912 | 10536 | 128 | 8/8 | 0 | 0 | 73.8 | 5948 | Same KM intro fingerprint; more balanced TILE/XOR |
| Foster The People – Don’t Stop… | AS | 299 | 299 | 1219 | 26530 | 96 | 12/12 | 0 | 2 | 48.3 | 601 | Double ladder + DEFTR; very XOR-heavy |
| Fall Out Boy – Thanks For The Memories | PHM | 0 | 0 | 732 | 22485 | 125 | 309/309 | 0 | 2 | 62.5 | 0 | **MEM₀ at pack 0**; extreme CLUT churn |
| America – Ventura Highway | NU | 0 | 4 | 5290 | 7394 | 144 | 9/9 | 0 | 0 | 79.4 | 0 | **CLUT → BORDER before first MEM** |
| Jeff Healy – While My Guitar… | SGB | 114 | 133 | **0** | 11702 | 80 | 1/1 | 0 | 0 | 84.6 | 2525 | **XOR-only** picture body |
| Vance Joy – Riptide | BH | 120 | 19 | **0** | 14856 | 416 | 1/1 | 0 | 0 | 76.1 | 697 | **XOR-only**; MEM color 15 ladder |
| Pink Floyd – Time | AH | 299 | 302 | 2338 | 15019 | 304 | 389/389 | **192** | 0 | 78.2 | 3382 | **Only** major **SCROLL** user here |
| Weezer – Troublemaker | CB | 87 | 88 | **13863** | 4135 | 464 | 52/**1** | 0 | 0 | 66.2 | 1007 | **Tile-heavy**; **single CLUT8** at EOF (anomaly) |
| Foster The People – Helena Beat | KV | 0 | 0 | 5308 | 23594 | 160 | 3/3 | 0 | 0 | 64.4 | 120 | **Loading bar**: row-1 strip + row-14 XOR sweep (§6.7) |
| VSHPARTY-109 – David Bowie – Let’s Dance | VSH | 0 | 314 | 800 | 13248 | 281 | 418/418 | 0 | 12 | 80.9 | 0 | **MEM repeat 2–8** open (not 8–15), long idle, **MEM₀ @314**; huge CLUT churn |
| ZPCP2023-2-15 – The Weeknd – Die for You | ZPCP | 300 | 300 | 1048 | 30182 | 160 | 81/81 | 0 | 28 | 60.8 | 2 | **Zoom 2023** catalog; **300 idle → MEM₀** + CLUT/BD/DEF block like **[AS]** |
| MBH20182-105 – Ariana Grande – No Tears… | MBH18 | 0 | 2 | 686 | 18493 | 121 | 3/4 | 0 | 1 | 72.9 | 0 | **CLUT → BORDER(8) → MEM**; ladder skips **repeat 9–11** |
| MBFP201-712 – Marshmello & Anne-Marie – Friends | MBFP | 0 | 1 | 746 | 18807 | 100 | 3/3 | 0 | 1 | 69.7 | 0 | **BORDER(8) before MEM₀**; same ladder **skip 9–11** as MBH20182 |
| 1975 – Chocolate | ME | 300 | 304 | 598 | 21296 | 128 | 4/4 | 0 | 1 | 68.9 | 379 | **Mr Entertainer**; **300 idle** then **MEM₀ @304** (same class as **ZPCP / Foster AS**) |
| Rod Stewart – Young Turks | MM | 312 | 315 | 6914 | 20996 | 112 | 125/125 | 0 | 0 | 64.1 | 1187 | **Music Maestro**: row-**1** **TILE** strip then **XOR** on same tiles (black-field + palette reveal) |
| Weezer – Hash Pipe | PHM | 15 | 618 | 6474 | 12721 | 288 | 7/7 | **296** | 7 | 67.0 | 2180 | **Scroll** ops bunched **near EOF** (~pack **56k+**); late **MEM color 7** block |
| Metric – Breathing Underwater | SN | 300 | 304 | 1210 | 6519 | 400 | 214/214 | 0 | 1 | 88.2 | 2 | **Sing It Now**: very high **MEM+CLUT** count → **fade / palette morph** friendly |
| Loverboy – Working For The Weekend | KB | 11 | 610 | 8079 | 12876 | 735 | 29/29 | 0 | 29 | 69.0 | 1094 | **KaraokeBay**: **late MEM color 15** block (**~610**); **DEF** paired with CLUT cycles |
| Bruce Springsteen – Hungry Heart | MH | 80 | 82 | **0** | 7844 | 400 | 1/1 | 0 | 0 | 87.3 | 2346 | **Monster Hits**: **MEM color 12** clear + **XOR-only** paint |
| Chicago – 25 Or 6 To 4 | P | 335 | 339 | 4872 | 8563 | 64 | 32/32 | 0 | 0 | 80.3 | 849 | **Pioneer**: tiles **alternate rows 1 & 16** → **dual horizontal bands** (“half screen” lyrics) |
| Stone Temple Pilots – Interstate Love Song | AH | 118 | 120 | **0** | 5066 | 80 | 1/1 | 0 | 0 | 91.7 | **7131** | **All Hits**: **XOR-only**; **no scroll**; **huge** trailing null run |

Where **TILE = 0** but XOR is nonzero, the file uses **only `TILE_BLOCK_XOR`** for drawing (decoder still uses the tile path).

---

## 5. Software / pipeline clustering (hypotheses)

These are **fingerprints**, not proof of shared binaries:

| Cluster | Observable signature | Example labels |
|--------|----------------------|----------------|
| **A – Zoom “Z-open”** | Packs 0–7: **MEM same color, repeat 8–15**; large **`command==0` gap**; first **MEM₀** ~300 packs in; often **high DEF** + **high XOR** | **Z** (Eagles, ELO), **L** shares Z-open + second-stage block |
| **B – KaraokeMaker “KM still”** | Short idle; **MEM 15 ladder**; **CLUT/BORDER** sandwich; **TILE+XOR** interleave | **KM** |
| **C – DK long-idle + dense timeline** | **324**-pack idle (matches Cyndi test asset); heavy **TILE+XOR** + **huge CLUT** counts | **DK** |
| **D – Modern XOR-first MP3+G** | **XOR-only** or **XOR dominates**; minimal CLUT count | **BH**, **SGB** |
| **E – Chart-style AS block** | ~**299** idle; **double MEM ladder** + **CLUT** + **BORDER** + **DEFTR** + column/row tile sweeps | **AS** |
| **F – Catalog partial-Z tail** | File opens with **MEM repeat 11–15 or 13–15** only, then idle, late **MEM₀** | **MF**, **MBH** |
| **G – PHM “instant clear”** | **MEM₀ at byte 0**; enormous **CLUT** updates (lyric coloring) | **PHM** |
| **H – NU “CLUT first”** | **CLUT → BORDER → idle → MEM** (border **before** clear ladder) | **NU** |
| **I – Chartbuster-style tile classic** | **TILE ≫ XOR**; modest CLUT; oddities possible | **CB** |
| **J – All Hits “Time” outlier** | **SCROLL_COPY** + **very high CLUT** + big tail | **AH** (*Pink Floyd – Time* only in this corpus) |
| **K – VocalHits / VSHPARTY** | **MEM** ladder begins at **low repeat (2–8)**, not **8–15**; long idle; **MEM₀** late; **very high CLUT** | **VSHPARTY** |
| **N – Zoom 2023 ZPCP block** | **~300-pack idle**, **MEM₀** immediately after idle, **CLUT+BD+DEF**, then tiles | **ZPCP** (same shape as **[AS]** / **ME Chocolate**) |
| **M – KaraokeBay late-KM** | **~600-pack** timeline before first **MEM color 15** clear; **DEF** with CLUT cycles | **KB** |

---

## 6. Per-track notes (this survey batch + prior threads)

### 6.1 Zoom Karaoke **[Z]** and **ZPCP** catalog

- **ELO – Evil Woman [Z]**: **Identical structural open** to **Eagles – Peaceful Easy Feeling [Z]** — **MEM₀** at pack **308**, **Z-repeat 8–15** at file head, **no trailing all-null tail** on disk.
- **Eagles [Z]** (prior): first **MEM₀** at **308** after **MEM 8–15** + long idle — canonical **Z-open** seek stressor.
- **ZPCP2023-2-15 – The Weeknd – Die for You** (§6.15): **Zoom 2023** product code — uses the **~300 idle → MEM₀** block pattern (**cluster N**), **not** classic **Z-open 8–15**.

### 6.2 Nutech **[NU]**

- **America – Ventura Highway [NU]**: **No leading idle**; pack **0** is **CLUT0**, **1** **CLUT8**, **2** **BORDER**, **3** idle, **4+** **MEM color 4** ladder from **repeat 0**. **Rare ordering**: **palette + border before first clear ladder**.

### 6.3 Pop Hits Monthly **[PHM]**

- **Fall Out Boy – Thanks For The Memories [PHM]**: **First pack is already `MEM repeat==0`** — **instant full clear**; **no** leading idle. **~309×** each CLUT half — **lyric/highlight palette animation** density far above typical still masters.
- **Weezer – Hash Pipe [PHM]**: second sample — **scroll is not an intro effect** here; see §6.19.

### 6.4 Chartbuster **[CB]** (inferred)

- **Weezer – Troublemaker [CB]**: **Tile-heavy** (**TILE 13.8k** vs **XOR 4.1k**). **Only one `CLUT8` in the entire file** (at pack **53240**, near EOF) vs **52 `CLUT0`** — **highly abnormal** for balanced 16-color work; likely **encoder bug**, **intentional “low 8 only” authoring**, or **rip artifact**. Worth flagging in QA.

### 6.5 MBH catalog **[MBH]**

- **MBH2022-108 – Ed Sheeran – Shivers**: Opens with **MEM repeat 13, 14, 15** only (partial **Z-tail**), then **long idle**, **`MEM₀` at 307** — **hybrid** between **Z timing pad** and **DK-like** long quiet run.

### 6.6 Music Factory **[MF]**

- **Layla [MF]**: Opens **MEM repeat 11–15**, then **long idle**, **`MEM₀` at 311** — same **partial-ladder tail** family as **MBH**, with **very high `cmd==0` fraction** (**85%**).

### 6.7 Karaoke Version **[KV]**

- **Foster The People – Helena Beat [KV]**: Standard **MEM ladder 0–15 from pack 0**, **BORDER**, **CLUT**, then **horizontal strip**: **`TILE` row 1 ~col 19**, then **`TILE_XOR`** marching columns **20+** on **row 1** — classic **progress / “loading bar”** illusion. Later packs show **`TILE_XOR` on row 14** advancing columns (**~635+**), consistent with a **secondary progress** or **wipe** element.

### 6.8 All Hits **[AH]** — same brand, different engines

- **Pink Floyd – Time [AH]**: **192** **`SCROLL_COPY`** ops; **lead idle 299**; **`MEM₀` @302**; **389×** each CLUT half; **3382** trailing null packs. Scrolls are **interleaved** through the timeline (vertical stepping in packet dumps). **Outlier** vs almost all other rips here.
- **Stone Temple Pilots – Interstate Love Song [AH]**: **No `SCROLL_*`**, **`TILE = 0` / XOR-only** body, **`MEM₀` @120**, **91.7%** `cmd==0`, **7131** trailing null packs — **closer to BH/SF “quiet wire”** than to **Pink Floyd Time**.

### 6.14 VocalHits **VSHPARTY-109** (David Bowie – Let’s Dance)

- Opens with **`MEMORY_PRESET` color 0, repeat 2, 3, … up to 8** (seven packs), then **~294** `cmd==0` packs, then **`MEM₀` @314** and a **second** full **0…15** ladder after **CLUT**. **Not** the canonical **Z-open (8–15)** pattern — treat as a **different “timing pad” variant**.
- **418×** each CLUT half — extreme **palette / lyric** traffic (Vocal Hits / party series look).

### 6.15 Zoom **ZPCP2023** (The Weeknd – Die for You)

- **300** leading idle packs; pack **300** is **`MEM repeat==0`** (starts clear immediately after idle).
- Sequence after ladder: **CLUT → BORDER → DEFTRANSPARENT → TILE** column strip — same **macro shape** as **Foster [AS]** and **1975 Chocolate [ME]** (§6.17).

### 6.16 Mr Entertainer **MBH20182** (Ariana Grande – No Tears Left to Cry)

- Pack **0: CLUT**, **1: BORDER (8)**, **2: `MEM₀`**, then ladder — but **repeat indices jump 8 → 12** (missing **9, 10, 11** on wire). Same **skip** appears on **MBFP201-712 – Friends** (**BORDER 8** then **`MEM₀` @1**, then **8,12,13,…**). Likely **shared encoder** between `MBH20182-*` SKUs and **MBFP201-712**.

### 6.17 Mr Entertainer **[ME]** (1975 – Chocolate)

- **300** idle packs; **`MEM₀` @304** — aligns with **ZPCP2023** / **Foster [AS]** “~300 idle then structured intro block” cluster (**N** in §5).

### 6.18 Music Maestro **[MM]** (Rod Stewart – Young Turks)

- After **~312** idle packs: **CLUT → BORDER(0) → MEM ladder → row-1 `TILE_BLOCK`** filling columns **1…~30**, then **`TILE_XOR`** on the **same (row,col)** cells — matches the **“black tiles / black palette then CLUT to reveal + flash logo”** description (luma/colour comes alive when **palette entries** change even if tile indices were already written).

### 6.19 Pop Hits Monthly **[PHM]** — second sample (Weezer – Hash Pipe)

- **Lead idle 15** then **CLUT/DEF** and a **long** idle run; first **`MEM₀` with clear colour 7** lands around pack **618** (second intro block).
- **296** scroll instructions exist but dumps show them **clustered near EOF** (~pack **56 600+**) — **end-of-show / credits scroll**, not the intro “loading bar” class.

### 6.20 Sing It Now **[SN]** (Metric – Breathing Underwater)

- **300** idle → **`MEM₀` @304**; **400** `MEMORY_PRESET` ops and **214×** each CLUT half — **very sparse `cmd==9` timeline** (**88%** `cmd==0`) with **heavy palette + preset churn** → consistent with **slow fades / cross-fades implemented as CLUT morphs**.

### 6.21 KaraokeBay **[KB]** (Loverboy – Working For The Weekend)

- Only **11** leading idle packs, but the **first full clear to colour 15** happens at pack **610** after **CLUT+DEF** cycles — **late “KM-colour” wall** similar to **KM still** authoring, just **pushed late** in the packet timeline.

### 6.22 Monster Hits **[MH]** (Bruce Springsteen – Hungry Heart)

- **80** idle packs → **CLUT → `MEM` colour 12 (`repeat==0`…) → BORDER(12) → XOR-only** column painting — **no plain `TILE_BLOCK`** in the whole file (`TILE=0`).

### 6.23 Pioneer **[P]** (Chicago – 25 Or 6 To 4)

- **335** idle packs; **`MEM₀` @339**.
- Early **tile placement alternates rows 1 and 16** while sweeping columns — **two horizontal bands**, matching the **“lyrics only in half the screen at a time”** experience on **1994-era** Pioneer / **LDCA**-style masters (user note).

### 6.9 Prior deep dives (conversation archive)

- **Toronto [KM] / Hip [KM]**: **KM ladder** color **15**, repeated **CLUT/BORDER/MEM** cycles; **Paramore MBFP**: **long idle**, **column-first** TILE+XOR; **Tom Petty [SF]**: **131** idle, **XOR wall** after MEM; **Black Velvet [DK]**: **324** idle, **dual MEM passes** with **31-pack gap**, **no DEFTR**; **Foster AS**, **Jackson L** (Z-open + second block); **Healy SGB** XOR-only body; **Riptide BH** XOR-only; **Cyndi DK** seek test reference (**324** idle).
- **Second survey batch (§6.14–§6.23)**: **VSHPARTY**, **ZPCP2023**, **MBH20182/MBFP** ladder skip, **ME Chocolate**, **MM Young Turks**, **PHM Hash Pipe** EOF scrolls, **SN Metric**, **KB Loverboy**, **MH Springsteen**, **P Chicago**, **AH STP**.

---

## 7. Engineering implications for `dashcdg`

1. **Seek must not depend on `process_packet` returning “dirty”** — Long **`command==0`** and **MEM repeat≠0`** both advance **`state->ts`** without painting (`core/src/cdg.c`, `dashcdg_cdg_reader_seek`).
2. **Keyframes** — Only **MEM repeat==0** creates keyframes (`dashcdg_cdg_reader_build_keyframes`). **Backward seek** before the first keyframe must **not** restore a keyframe **after** the target; **`dashcdg_find_closest_keyframe`** now returns **NULL** when all keyframes are **strictly after** `ts` (regression: `test_reader_seek_backward_before_first_keyframe`).
3. **Subchannel trim** — `dashcdg_cdg_compute_subchannel_trims` corrects **24-byte phase**; it does **not** remove **thousands of trailing null packs** — CDG duration can **exceed** MP3 unless the player clamps to audio.
4. **Throughput** — **Z**, **PHM**, **AS**, **BH**, **SGB**, **ZPCP**, **MH**, **STP [AH]** paths imply **sustained XOR tile** decoding; embedded targets must budget **raster dirty** merges accordingly (`cdg_batch_jitter`, badge UI).
5. **EOF behaviour** — Some files (**Hash Pipe [PHM]**, **STP [AH]**) attach **thousands** of **`cmd==0`** or **scroll** packs at **EOF**; players that drive UI off **CDG packet index** vs **audio clock** can show **frozen / scrolling credits** after the last lyric unless clamped to MP3 duration.

---

## 8. Unknowns and follow-ups

| Tag | Status |
|-----|--------|
| **AH (All Hits)** | **Pink Floyd – Time** is the **only** **[AH]** sample here with **heavy scroll**; **STP – Interstate Love Song [AH]** disproves a single “All Hits = scroll” rule. Add more **[AH]** tracks to cluster. |
| **CB** | Treated as **Chartbuster** per industry naming; **CLUT8 anomaly** on Weezer **Troublemaker** should be re-checked on a second **CB** rip if available. |
| **MBH / MBH20182 / MBFP** | `MBH20182-*` ladder skip + BORDER-first matches **MBFP201-712** — likely **shared Mr Entertainer / chart-pack pipeline**; still verify against additional SKUs. |
| **VSHPARTY** | Decoded as **Vocal Hits / party** line from filename; no independent vendor link in-repo. |
| **DC** | No file in this pass. |
| **ZPCP** | Zoom **2023** product code; fingerprint aligns with **AS/ME** “300-idle block” (**cluster N**) — treat as **Zoom modern MP3+G** line. |

---

## 9. In-repo references

- `core/src/cdg.c` — `MEMORY_PRESET`, seek, keyframes, scroll, tiles.
- `docs/specs/cdg-subchannel-alignment.md` — **`0x09`** graphics channel.
- `docs/specs/v4-live-video-playout.md` — v4 CDG batching / anchors (TX).
- `tests/test_core.c` — `test_reader_seek_*` family.

---

*Document generated from on-disk analysis in the dashcdg workspace; extend the metrics table as new branded samples are added to `cdg/`.*
