<#
.SYNOPSIS
  PowerShell-only: activate ESP-IDF, build, flash, then capture UART — never at the same time on one COM port.

.DESCRIPTION
  Uses C:\Espressif\Initialize-Idf.ps1 (Path A). No cmd.exe batch step. After esptool releases the port,
  waits and retries opening the serial port before logging.

  From Cursor/bash, prefer the **full Windows PowerShell 5.1** binary so `System.IO.Ports` resolves:
    C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -File "...\dashcdg\scripts\idf_flash_then_capture_badge.ps1"

.PARAMETER Port
  Serial port (default COM6).

.PARAMETER CaptureSeconds
  How long to read UART after flash completes (default 45).

.PARAMETER LogPath
  Output log file; default scripts/badge_uart_last.txt next to this script.

.PARAMETER SkipFlash
  Only capture UART (use after a manual flash if you only need logs).

.PARAMETER PostFlashWaitSeconds
  Initial delay after flash before opening the serial port (default 5).
#>
param(
    [string]$Port = "COM6",
    [int]$CaptureSeconds = 45,
    [string]$LogPath = "",
    [switch]$SkipFlash,
    [int]$PostFlashWaitSeconds = 5,
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

foreach ($name in @(
        "MSYSTEM", "MSYS2_ROOT", "MINGW_PREFIX", "MINGW_CHOST", "MINGW_PACKAGE_PREFIX",
        "CYGWIN", "CHERE_INVOKING", "SHELL", "TERM", "PWD", "OLDPWD")) {
    if (Test-Path -LiteralPath "Env:$name") {
        Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    }
}

& "C:\Espressif\Initialize-Idf.ps1" -IdfId "esp-idf-20ee62e792ea89630ac6a777ab3ebc57"

$badgeDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\platform\espidf\projects\dashcdg_badge"))
if (-not (Test-Path -LiteralPath $badgeDir)) {
    throw "Badge project directory not found: $badgeDir (fix repo layout or run from scripts folder)."
}
Set-Location -LiteralPath $badgeDir

if (-not $LogPath) {
    $LogPath = Join-Path $PSScriptRoot "badge_uart_last.txt"
}

if (-not $BuildDir) {
    $BuildDir = Join-Path $env:TEMP "dashcdg_badge_build"
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

if (-not $SkipFlash) {
    idf.py --version
    Write-Host "Building and flashing on $Port (exclusive; close any monitor on this port)..."
    idf.py -B $BuildDir -p $Port build flash
    Write-Host "Flash finished. Waiting ${PostFlashWaitSeconds}s for esptool to release $Port..."
    Start-Sleep -Seconds $PostFlashWaitSeconds
} else {
    Write-Host "SkipFlash: capturing UART only."
    Start-Sleep -Seconds 1
}

Write-Host "Capturing UART $Port for $CaptureSeconds s -> $LogPath"

function Import-DashcdgSerialPortAssembly {
    if ('System.IO.Ports.SerialPort' -as [type]) {
        return
    }
    $dllCandidates = @(
        (Join-Path $PSHOME 'System.IO.Ports.dll')
        (Join-Path ([System.Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()) 'System.IO.Ports.dll')
    )
    foreach ($dll in $dllCandidates) {
        if ($dll -and (Test-Path -LiteralPath $dll)) {
            try {
                [void][System.Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $dll).Path)
                if ('System.IO.Ports.SerialPort' -as [type]) {
                    return
                }
            } catch {
                continue
            }
        }
    }
    try {
        Add-Type -AssemblyName System.IO.Ports -ErrorAction Stop
        return
    } catch {
        try {
            Add-Type -AssemblyName System -ErrorAction Stop
            if ('System.IO.Ports.SerialPort' -as [type]) {
                return
            }
        } catch {
            # ignore
        }
    }
    throw "Cannot load System.IO.Ports (needed for SerialPort). If you use PowerShell 7+, ensure $(Join-Path $PSHOME 'System.IO.Ports.dll') exists; otherwise run this script with Windows PowerShell 5.1 (powershell.exe from System32)."
}

Import-DashcdgSerialPortAssembly

$sp = New-Object System.IO.Ports.SerialPort(
    $Port,
    115200,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 50
$sp.WriteTimeout = 50

$openDeadline = [datetime]::UtcNow.AddSeconds(20)
$opened = $false
while (-not $opened) {
    try {
        $sp.Open()
        $opened = $true
    } catch {
        if ([datetime]::UtcNow -gt $openDeadline) {
            throw "Could not open $Port after flash (still busy or wrong port): $($_.Exception.Message)"
        }
        Write-Host "Port not ready yet, retrying open on $Port..."
        Start-Sleep -Milliseconds 400
    }
}

try {
    $fs = [System.IO.File]::Open($LogPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $buf = New-Object byte[] 8192
        $deadline = [datetime]::UtcNow.AddSeconds($CaptureSeconds)
        while ([datetime]::UtcNow -lt $deadline) {
            if ($sp.BytesToRead -gt 0) {
                $toRead = [Math]::Min($buf.Length, $sp.BytesToRead)
                $n = $sp.Read($buf, 0, $toRead)
                if ($n -gt 0) {
                    $fs.Write($buf, 0, $n)
                }
            } else {
                Start-Sleep -Milliseconds 15
            }
        }
    } finally {
        $fs.Close()
    }
} finally {
    $sp.Close()
}

Write-Host "Done. Log: $LogPath"
Write-Host "Tip: grep AUDIO_UART_PROOF / audio_chop / dac_dma in the log for DMA health."
