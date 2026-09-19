# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Samuele Voltan
#
# Launch lba2.z64 in the Ares emulator with the ROM's ISViewer debug output
# (boot trace, engine LogPrintf, libdragon asserts + backtraces) captured to
# ares_log.txt next to the ROM. This log is the port's main diagnostic channel.
#
# Usage:
#   .\tools-n64\run-ares.ps1                 -> runs .\lba2.z64
#   .\tools-n64\run-ares.ps1 path\to\rom.z64
#
# Set ARES to the ares.exe path (default: "ares" on PATH). Ares pauses
# emulation while its window is in the background.

$ErrorActionPreference = "Stop"

$rom = if ($args.Count -ge 1) { $args[0] } else { Join-Path (Get-Location) "lba2.z64" }
$ares = if ($env:ARES) { $env:ARES } else { "ares" }

if (-not (Test-Path $rom)) { throw "ROM not found: $rom" }

Stop-Process -Name ares -Force -ErrorAction SilentlyContinue

$hash = (Get-FileHash $rom -Algorithm MD5).Hash.ToLower()
Write-Host "[run-ares] $rom  md5=$hash" -ForegroundColor Cyan

$dir = Split-Path -Parent (Resolve-Path $rom)
$log = Join-Path $dir "ares_log.txt"
$logErr = Join-Path $dir "ares_log.err.txt"
Start-Process -FilePath $ares -ArgumentList "`"$rom`"" `
    -RedirectStandardOutput $log -RedirectStandardError $logErr
Write-Host "[run-ares] Ares launched (ISViewer log -> $log)" -ForegroundColor Green
