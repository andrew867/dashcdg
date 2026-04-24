# `third_party/` (local upstream trees)

Git ignores almost everything here so clones stay small. After you run:

```bash
bash scripts/bootstrap_esp_idf.sh
```

you will have:

- `third_party/esp-idf/` — Espressif **ESP-IDF** (includes **FreeRTOS** under `components/freertos`, LWIP, Wi‑Fi stack, etc.)

Copy this entire folder to another PC or USB drive for offline work (together with the Espressif tools directory; see `docs/embedded/esp-idf-setup.md`).
