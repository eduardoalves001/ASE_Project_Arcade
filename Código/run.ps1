# run.ps1 - convenience wrapper so you can launch the dashboard from PowerShell.
#
# run.sh is a bash script; PowerShell cannot execute it directly. This finds the
# Git Bash "bash.exe" and runs run.sh through it, forwarding any arguments.
#
#   .\run.ps1                 # auto-detect Wi-Fi IP, full launch
#   .\run.ps1 --ip 10.0.0.5   # force the broker IP the ESP32 uses
#   .\run.ps1 --no-broker     # dashboard only
#   .\run.ps1 --setup-only    # regenerate certs/config only
#
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Prefer Git for Windows' bash explicitly. A bare "bash.exe" on PATH is often
# WSL's launcher, which uses /mnt/c/... and would not find /c/... script paths.
$bash = $null
foreach ($cand in @(
    "$env:ProgramFiles\Git\bin\bash.exe",
    "${env:ProgramFiles(x86)}\Git\bin\bash.exe",
    "$env:LOCALAPPDATA\Programs\Git\bin\bash.exe")) {
    if (Test-Path $cand) { $bash = $cand; break }
}
if (-not $bash) { $bash = (Get-Command bash.exe -ErrorAction SilentlyContinue).Source }
if (-not $bash) {
    throw "Git Bash (bash.exe) not found. Install Git for Windows from https://git-scm.com"
}

Write-Host "Launching run.sh through Git Bash... (Ctrl+C to stop)" -ForegroundColor Cyan
# bash.exe needs a POSIX path for the script file, e.g. C:\foo -> /c/foo.
$drive  = $here.Substring(0, 1).ToLower()
$script = "/$drive" + ($here.Substring(2) -replace '\\', '/') + "/run.sh"
& $bash $script @args