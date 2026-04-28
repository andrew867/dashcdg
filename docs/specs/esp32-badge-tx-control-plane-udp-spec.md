# ESP32 badge ↔ desktop TX control-plane UDP — specification, RCA, and test plan

**Status:** Draft + **as-built update (2026-04)** — product path now splits repair NACK from peer stats multicast; see §2.1.  
**Scope:** `V4_RX_STATS`, `V4_REPAIR_NACK`, PTP on the “stats” UDP port (default **24685**), plus interactions with media (**24684**) and split repair (**24686**).  
**Goal:** `V4_RX_STATS` remains on the **primary** multicast group for peer convergence; **NACKs** avoid flooding desktop-rx (unicast to TX when known, else `239.255.77.78:24685`); TX joins both groups in multicast mode.

---

## 1. Symptom summary

| Symptom | Context |
|--------|---------|
| Desktop **players** (rx) degraded when badge sent heavy NACK/stats | Multicast `239.255.77.77:24685` fan-out → every rx on LAN parsed datagrams |
| After **unicast-first** to last seen source IP, badge stats “don’t look like” they reach TX | Likely **TX not receiving** unicast, or **observers** (Wireshark on group) no longer see traffic, or **learned IP never set** |

These are **different failure modes**; fixes must not trade one for the other.

---

## 2. Architecture (as-built, relevant paths)

### 2.1 Addresses and ports (defaults)

| Path | Multicast group | Port | Direction |
|------|------------------|------|-------------|
| v4 media | `239.255.77.77` | 24684 | TX → all receivers |
| v4 repair (split) | `239.255.77.77` | 24686 | TX → receivers that joined repair |
| v4_rx_stats + PTP | `239.255.77.77` | 24685 | Badge + desktop-rx → all peers; TX listens |
| v4_repair_nack | **`239.255.77.78`** or **unicast TX** | 24685 | Badge (and desktop-rx) → TX only path; TX joins `.78` in multicast mode |

### 2.2 Desktop TX (`app_tx.c`)

- Binds `ptp_sockfd` to `INADDR_ANY:rx_stats_port` (default **24685**).
- If endpoint is multicast, **joins** the media multicast group on that socket (same group address as media for PTP/control).
- Receives **unicast** to any local IPv4 on 24685 and **multicast** copies to 24685.

**Implication:** Unicast to the TX host’s **primary** IPv4 on port 24685 **should** work if routing/firewall/AP policy allows STA→host UDP.

### 2.3 ESP32 badge (`badge_rx.c`)

**Learned TX IPv4 (`s_v4_tx_src_ipv4`):**

| Source socket | Updates `s_v4_tx_src_ipv4`? | Condition |
|---------------|-----------------------------|-----------|
| Media `s_sock` (24684) | **Yes** | After `recvfrom`, if `badge_rx_ipv4_is_unicast_src(src)` |
| Stats `s_stats_sock` (24685) | **No** | Only counts peer `V4_RX_STATS` with different `receiver_instance_id` |
| Repair `s_repair_sock` (24686) | **Yes** | Same unicast check as media |

**Control send (`badge_rx_fill_tx_control_sockaddr`):**

- If `s_v4_tx_src_ipv4 != 0` → `sendto` **unicast** that address, port 24685.
- Else → `sendto` **multicast** `239.255.77.77`, port 24685.

### 2.4 Desktop RX (`app_rx.c`)

- Optional stats sender + **stats listener** on 24685 (default stats interval 2000 ms).
- Fast-path skips full parse unless wire type byte is `V4_RX_STATS` (reduces CPU when multicast still used).

---

## 3. Root cause analysis (NASA-style fault tree)

### 3.1 Branch A — “TX doesn’t see badge stats” (unicast path)

| ID | Hypothesis | Likelihood | Evidence / test |
|----|-------------|------------|-----------------|
| A1 | **`s_v4_tx_src_ipv4` never becomes non-zero** | Medium | No media/repair RX yet; or LwIP never delivers unicast SA on multicast RX (platform verify) |
| A2 | **Learned IP is not the TX host** (spoof / wrong peer / NAT) | Low–medium | Another device sends to same group first; NAT rewrites SA inconsistently |
| A3 | **STA↔STA or client isolation** — unicast between Wi‑Fi clients dropped; multicast flooded by AP | Medium on consumer APs | Repro: TX on Wi‑Fi same AP as badge; unicast fails, multicast succeeds |
| A4 | **OS firewall** — allows multicast subscription traffic but blocks unicast UDP 24685 from Wi‑Fi subnet | Medium on Windows | Repro: allow rule fixes; tcpdump on TX shows no packets |
| A5 | **Wrong port** — TX uses non-default `--tx-rx-stats-port`; badge hardcodes `BADGE_RX_TX_STATS_PORT` | Low | Config mismatch in field |
| A6 | **sendto succeeds but packet never arrives** (routing, reverse path) | Low | Capture on TX NIC |

**Most credible combined failure:** **A1 + A3/A4** — badge switches to unicast before a **trusted** TX IP is learned, or unicast is **dropped on LAN** while multicast was **observed** by everyone including TX.

### 3.2 Branch B — “Observers think stats stopped” (visibility)

| ID | Hypothesis | Likelihood |
|----|------------|------------|
| B1 | Wireshark subscribed only to **multicast** group; unicast no longer visible | High (expected) |
| B2 | TX HUD shows “reporters” keyed by IP; unicast source is badge — should still count if received | Low if receive works |

### 3.3 Branch C — “Players break again” (regression)

| ID | Hypothesis | Likelihood |
|----|------------|------------|
| C1 | Fallback multicast storms return under loss | Medium if we “fix” by blasting both |
| C2 | Dual-send doubles airtime and jitter | High if always unicast+multicast |

### 3.4 Branch D — Future protocol / ops risks

| ID | Risk |
|----|------|
| D1 | No **authoritative** `control_plane_ipv4` in `V4_SESSION_INFO` — learning is implicit and fragile |
| D2 | Multi-homed TX (Wi‑Fi + Ethernet) — SA seen by badge may not be the interface TX’s OS accepts for 24685 |
| D3 | IPv6 / dual-stack not represented (out of scope today; document) |
| D4 | **Rate** of NACK/stats under loss — control plane can still starve Wi‑Fi even if unicast |

---

## 4. Requirements specification (behavioral)

### 4.1 Functional

1. **R-F1 (TX delivery):** Under default ports and multicast media, the TX process **must** receive `V4_RX_STATS` from a badge at **≥99%** of the badge’s configured report interval in steady state (see test matrix).
2. **R-F2 (Player isolation):** With **N** desktop-rx instances on the LAN, adding one badge **must not** increase per-rx CPU or recv rate on 24685 by more than **ε** compared to badge absent, when badge uses **approved** control-plane strategy (ε defined in perf test: e.g. < 5 extra parses/sec/rx).
3. **R-F3 (NACK delivery):** `V4_REPAIR_NACK` **must** reach TX with same delivery class as stats (unicast preferred when TX IP trusted).
4. **R-F4 (Cold start):** Before any trusted TX SA is known, control **may** use multicast fallback **or** delay first stats until session anchor (policy choice — pick one in §6).

### 4.2 Trust model for “TX IP”

Define **trusted control destination** `tx_ctrl_ipv4_be`:

| Tier | Source | Action |
|------|--------|--------|
| T0 | None known | Multicast fallback **or** hold sends (policy) |
| T1 | Unicast SA from **media** (24684) or **repair** (24686) passing `badge_rx_ipv4_is_unicast_src` | Candidate TX IP |
| T2 | (Future) Explicit field in `V4_SESSION_INFO` or announce extension | Overrides T1 |

**R-T1:** Never set `tx_ctrl_ipv4_be` from **peer** `V4_RX_STATS` on 24685 unless payload is cryptographically tied to TX (not today) — **continue to forbid** learning from peer stats.

### 4.3 Non-functional

- **R-N1:** Worst-case extra control traffic vs “multicast-only” **must not** exceed agreed cap (e.g. no sustained dual-send unless policy says “probe only”).
- **R-N2:** Deterministic logging: one-line log on badge when switching T0→T1 (IP + reason) for field debug.

---

## 5. Design options (decision record)

| Option | Pros | Cons |
|--------|------|------|
| **O1** Multicast only (revert unicast) | Always reaches TX if joined | Floods all rx |
| **O2** Unicast only after first SA | No rx flood | Fails A3/A4/A1 |
| **O3** Unicast + **watchdog multicast** (e.g. every K stats or if “TX silent”) | TX likely receives | Some rx load; tune K |
| **O4** **Dual send** (unicast + multicast) always | Redundant | Airtime + rx CPU |
| **O5** Session/announce carries `control_ipv4` | Explicit, AP-agnostic | Protocol + TX change |

**Recommendation (phased):**

- **Phase 1 (no wire change):** O3-lite — **unicast when T1 valid**; if `sendto` fails **or** optional **heartbeat**: one multicast every **N** seconds **only while** TX stats not observed by badge (requires feedback — badge doesn’t know TX saw stats → use **time-since-last-media** or **session seq** only, or simpler: **multicast duplicate every M-th** packet only when `s_v4_tx_src_ipv4==0` — already have fallback).
- **Phase 2:** Add optional `v4_session_info.control_plane_ipv4` (uint32 BE, 0 = use legacy behavior) — **O5** for hardened deployments.

---

## 6. Test plan

### 6.1 Unit / host (no radio)

| Test ID | Description | Pass criteria |
|---------|-------------|---------------|
| UT-1 | Parse fast-path on rx stats thread | Non-stats type never calls full `parse` path (coverage or mock) |
| UT-2 | `badge_rx_ipv4_is_unicast_src` matrix | Class D multicast SA rejected; RFC1918 accepted |

### 6.2 Lab integration (Ethernet + Wi‑Fi)

| Test ID | Topology | Steps | Pass |
|---------|----------|-------|------|
| IT-1 | TX wired, badge Wi‑Fi, 1× rx | Run karaoke 10 min | TX reporter shows badge; rx CPU stable |
| IT-2 | TX Wi‑Fi, badge Wi‑Fi, **AP client isolation ON** | Same | TX receives stats (multicast fallback or dual strategy per §6) |
| IT-3 | TX Wi‑Fi, badge Wi‑Fi, isolation **OFF** | Same | Unicast-only acceptable if A3 ruled out |
| IT-4 | 3× desktop-rx + badge | Flood NACK (stress) | rx frame times < threshold; no audio underrun storm |
| IT-5 | Wireshark on **unicast** filter to TX IP | Badge reporting | Datagrams visible to TX IP:24685 |

### 6.3 Regression matrix (after code change)

| Build | Badge NVS stats | Expected |
|-------|-----------------|----------|
| TX default port | on | TX counters increment |
| TX `--tx-rx-stats-port` ≠ 24685 | on | **Must** match Kconfig/badge port or doc mismatch |

### 6.4 Automated (future)

- **Harness:** Two UDP sockets in test: “TX” bind :24685; “badge” send stats; assert RX count — with and without multicast join.
- **ESP-IDF:** Host-based test of `recvfrom` SA for multicast datagram (qemu or target log) — document LwIP behavior.

---

## 7. Implementation plan (ordered; execute after approval)

1. **Instrument** (temporary): log `s_v4_tx_src_ipv4`, `sendto` errno, and dst each stats/NACK (rate-limited).
2. **Verify** on failing site: tcpdump on TX for `host <badge_ip> and port 24685`.
3. **Choose policy** from §5 (recommend **O3-lite**: unicast + **periodic multicast echo** every 30s **only if** unicast path untested — or simpler immediate fix: **multicast duplicate** first stats after session join only).
4. **Protocol (optional Phase 2):** extend `V4_SESSION_INFO` with optional control IPv4; TX fills from bind interface or CLI `--tx-control-ip`.
5. **Docs:** operator note — “if TX doesn’t see badge stats, check firewall / AP isolation / port match.”
6. **Remove** excessive instrumentation after green IT matrix.

---

## 8. Traceability

| Artifact | Location |
|----------|----------|
| Badge control send / learn | `platform/espidf/projects/dashcdg_badge/main/badge_rx.c` |
| TX PTP / stats bind | `platform/desktop/src/app_rx.c` N/A; `app_tx.c` ~8685+ |
| Rx stats thread fast-path | `platform/desktop/src/app_rx.c` |
| Protocol enums | `proto/include/dashcdg/protocol.h` |

---

## 9. Sign-off checklist (before merge)

- [ ] IT-1 … IT-5 executed and logged  
- [ ] No sustained desktop-rx parse regression (profile or counter)  
- [ ] TX `v4_rx_stats_packets_received` increases with badge-only on LAN  
- [ ] Documented operator troubleshooting for isolation/firewall  

---

## 10. Open questions for product owner

1. Is **brief multicast** on cold start acceptable if it restores TX visibility on hostile APs?  
2. Is a **small protocol addition** (`control_plane_ipv4`) acceptable for v4.1, or stay wire-stable?  
3. Should badge **prefer** repair-socket SA over media-socket SA when split repair is on (same TX, usually identical)?

---

*End of planning document. No code changes were made in the commit that adds this file.*
