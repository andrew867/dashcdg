# ESP-IDF + FreeRTOS setup (dashcdg embedded)

This repo wires **ESP-IDF** as the supported path for ESP32 firmware. ESP-IDF already bundles **FreeRTOS** (`third_party/esp-idf/components/freertos` after bootstrap), so you do not maintain a separate FreeRTOS tree.

## Where to get ESP-IDF (official)

| Resource | URL |
| --- | --- |
| Getting started (ESP32) | [ESP-IDF Programming Guide — Get Started](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) |
| GitHub sources | [github.com/espressif/esp-idf](https://github.com/espressif/esp-idf) |
| Windows installer (alternative) | [Espressif IDE / ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/) *(installs toolchain + Python; then point `IDF_PATH` at your clone if you still use this repo’s project)* |

## One-command bootstrap (recommended for this repo)

From Git Bash at the repo root:

```bash
bash scripts/bootstrap_esp_idf.sh
```

This clones ESP-IDF into **`third_party/esp-idf`** (ignored by git, except `third_party/README.md`) and runs `install.bat esp32` on Windows or `./install.sh esp32` elsewhere to download **Xtensa toolchain**, **Python env**, and tools into `%USERPROFILE%\.espressif` (default).

Override the ESP-IDF tag/branch:

```bash
DASHCDG_ESP_IDF_REF=v5.5.4 bash scripts/bootstrap_esp_idf.sh
```

Default tag tracks current bring-up; align with the Espressif IDE or offline installer version you use (for example **v5.5.4**).

## Build firmware

Each new shell must load ESP-IDF into the environment (PATH, `IDF_PYTHON_ENV_PATH`, etc.):

```bash
source third_party/esp-idf/export.sh
bash scripts/build_esp32_freertos_platform.sh
```

The script sources `export.sh` for you. Firmware project: **`platform/espidf/projects/dashcdg_badge/`**.

That app targets the **Freenove CYD 3.2"** (ST7789 + XPT2046): **LVGL** status UI, on-screen keyboard, **touch-driven Wi-Fi** scan/connect, credentials in **NVS** — intentionally **no** HTTP captive portal / web config (saves flash and keeps bring-up simple).

The badge project uses a **custom partition table** (`partitions_ota_4mb.csv`) with **two OTA app slots** for future over-the-air updates. If you previously flashed a **different** table on the same chip, erase flash once (`idf.py erase-flash`) before flashing so **otadata** is consistent.

## Flash (UART, RTS/DTR auto-reset)

Default serial port **`COM6`** (override with first argument or `ESPPORT`):

```bash
bash scripts/flash_esp32_freertos.sh COM6
```

Esptool drives **RTS** / **DTR** as usual for ESP32 auto-reset and **GPIO0** boot-strapping; boards wired like your CYD devkit work with **one-click download** from the IDE or `idf.py flash`.

Optional serial monitor after flash:

```bash
DASHCDG_ESP_MONITOR=1 bash scripts/flash_esp32_freertos.sh COM6
```

## Offline / air-gapped workflow

Git does **not** store ESP-IDF (too large). For a fully offline second machine:

1. Run bootstrap once online.
2. Copy **`third_party/esp-idf/`** and the Espressif tools directory (default **`%USERPROFILE%\.espressif`** on Windows, or `$HOME/.espressif` on Unix).
3. On the offline machine, set **`IDF_TOOLS_PATH`** if you put tools somewhere non-default, then `source export.sh` and build as above.

See also Espressif docs on **offline installation** in the same Getting Started guide.

## Relation to dashcdg docs

- Embedded porting rules: [`README.md`](README.md), [`protocol-v4-porting-guide.md`](protocol-v4-porting-guide.md), [`freertos-esp32-implementation-plan.md`](freertos-esp32-implementation-plan.md).
- Desktop reference behavior: [`windows-desktop-reference.md`](windows-desktop-reference.md).

Next firmware milestones (Wi‑Fi AP/STA web UI, multicast, v4 RX) follow those documents; **`dashcdg_badge`** is the on-device shell to grow from.
