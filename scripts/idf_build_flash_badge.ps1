$ErrorActionPreference = "Stop"

# Drop Git Bash / MSYS env inheritance — ESP-IDF activation fails with "MSys/Mingw is not supported".
foreach ($name in @(
        "MSYSTEM", "MSYS2_ROOT", "MINGW_PREFIX", "MINGW_CHOST", "MINGW_PACKAGE_PREFIX",
        "CYGWIN", "CHERE_INVOKING", "SHELL", "TERM", "PWD", "OLDPWD")) {
    if (Test-Path -LiteralPath "Env:$name") {
        Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    }
}

& "C:\Espressif\Initialize-Idf.ps1" -IdfId "esp-idf-20ee62e792ea89630ac6a777ab3ebc57"

Set-Location "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\platform\espidf\projects\dashcdg_badge"

idf.py --version
idf.py -p COM6 build flash
