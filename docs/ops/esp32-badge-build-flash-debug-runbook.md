# ESP32 Badge Build/Flash/Debug Runbook (Windows)

This is the known-good workflow for `platform/espidf/projects/dashcdg_badge` on Windows.

## Preconditions

- ESP-IDF is installed under `C:\Espressif`.
- Badge is connected on the expected serial port (currently `COM6`).
- Use native `powershell.exe` or `cmd.exe`.
- Do not run ESP-IDF activation from Git Bash/MSYS shells.

## One-command build + flash (PowerShell)

Use the committed script:

`scripts/idf_build_flash_badge.ps1`

It does:

1. `Initialize-Idf.ps1` for `esp-idf-20ee62e792ea89630ac6a777ab3ebc57`
2. `cd` into `platform/espidf/projects/dashcdg_badge`
3. `idf.py --version`
4. `idf.py -p COM6 build flash`

Run it from native PowerShell:

```powershell
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\scripts\idf_build_flash_badge.ps1"
```

## Build only (CMD helper)

Use:

`scripts/idf_build_badge.cmd`

This is useful for quick compile checks without flashing.

## Post-flash debug cycle

Optional full automation switch:

- Enable `CONFIG_DASHCDG_BADGE_DEBUG_AUTO_LAUNCH_KARAOKE_AFTER_DHCP=y`
- Behavior: after first STA DHCP lease on each boot, badge opens Karaoke once; audio/video decode follow **Settings / NVS** (not overridden by this flag).

1. Install serial dependency (one-time):

```powershell
python -m pip install --user pyserial
```

2. Run one capture cycle:

```powershell
python "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\scripts\esp32_badge_debug_cycle.py" --skip-build-flash --iterations 1 --log-seconds 120 --port COM6
```

3. Wait for DHCP marker in logs, then launch CDG app on badge for playout validation.

4. Summarize capture:

```powershell
python "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\scripts\esp32_badge_log_summary.py" "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\docs\ops\logs\esp32-debug-cycle\<log-file>.log"
```

## Known failure mode and fix

- If activation fails with `MSys/Mingw is not supported`, the process inherited MSYS/Git-Bash environment variables.
- Fix: launch native `powershell.exe`/`cmd.exe` directly and run the scripts there.

