# Andrew Green — public engineering portfolio (snapshot)

**Contact:** [me@andrewgreen.ca](mailto:me@andrewgreen.ca) · **GitHub:** [@andrew867](https://github.com/andrew867) · **Site:** [andrewgreen.ca](https://andrewgreen.ca)

This page is a **one-screen** index of **public repos** I stand behind—useful for hiring managers, collaborators, or future-me. It lives in **dashcdg** because that’s where the docs culture is; it describes **other** work too.

---

## At a glance

| Axis | Examples |
| --- | --- |
| **Languages / runtime** | C99 (desktop + embedded), C# / .NET 8, build systems (Make, MSYS2, ESP-IDF), scripting for CI and soak |
| **Domains** | Real-time **audio/video** over UDP multicast, **protocol design**, **FEC / repair**, **sync & metrics**, **pro AV** (monitor control: SDAP/SDCP), **telecom heritage** tooling (BCM) |
| **Practices** | Versioned wire formats, long **soak** + jsonl metrics, **normative specs** beside code, tagged releases |

---

## Flagship repositories

### [dashcdg](https://github.com/andrew867/dashcdg) (this repo)

**Multicast karaoke transport** — CD+G timed with live audio on a **versioned UDP protocol (v4)**, desktop **TX/RX** (OpenGL, Win32 GDI, headless), **Opus** + narrowband codec matrix, **jitter buffers**, **bounded FEC**, cold join, enterprise-style **multi-receiver group sync**, and enough **Windows build matrices** to fill a sneakernet zip.

*Why it matters:* end-to-end **systems** work—protocol, codecs, graphics paths, Windows audio, embedded parity (ESP32), and **documentation** (`docs/README.md`, release notes, test plans).

### [MonitorControlSDK](https://github.com/andrew867/MonitorControlSDK)

**.NET 8** SDK and tooling for **Sony-compatible professional monitors** that speak **SDAP** (UDP discovery) and **SDCP** (TCP / UDP control): NuGet library, **`monitorctl`** CLI, optional **HTTP + OpenAPI** web UI, samples, **ESP32 native TCP SDCP**, Python gateway examples. MIT-licensed, CI-backed.

*Why it matters:* **vendor-adjacent integration** without hiding behind a black box—framing, transport, docs, and operator-facing surfaces.

### [BCM-Tools](https://github.com/andrew867/BCM-Tools)

Community **support tooling** for legacy **Nortel BCM** PBX—password-of-the-day, patching notes, tone/profile customization for people keeping **obsolete** gear out of landfills.

*Why it matters:* **reverse-engineering mindset**, clear warnings, and user-respecting documentation under messy real-world constraints.

---

## Other public repos (short)

| Repo | Note |
| --- | --- |
| **Forks / experiments** | Various—check [@andrew867/repos](https://github.com/andrew867?tab=repositories) for activity and stars. |
| **Upstream I rely on** | e.g. [hharte/mm_manager](https://github.com/hharte/mm_manager) (Millennium payphone manager)—I am **not** the author; useful context for telecom interest. |

*(Add rows here when you want a new “pinned” project—keep this file honest.)*

---

## How I work

Specs and **test plans** before—or alongside—big behavior changes; **metrics** when arguing with reality; **releases** and **tags** when something is actually shippable. If your team values **plain-language docs** and **soak logs** over slide decks, we’ll get along.

---

*Last updated: 2026-04-30 — bump the date when you add a major repo or role.*
