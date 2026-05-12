<#
.SYNOPSIS
  PowerShell-only: ESP-IDF build + flash (no UART capture). For build + flash + log, use idf_flash_then_capture_badge.ps1.
.DESCRIPTION
  Use -BuildOnly from automation when you only need `idf.py build` (no serial port / no esptool flash).
#>
param(
    [string]$Port = "COM6",
    [switch]$BuildOnly
)

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

$badgeDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\platform\espidf\projects\dashcdg_badge"))
Set-Location -LiteralPath $badgeDir

idf.py --version
if ($BuildOnly) {
    idf.py build
} else {
    idf.py -p $Port build flash
}
