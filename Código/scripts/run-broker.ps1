<#
.SYNOPSIS
  Starts the ESP-Arcade Mosquitto TLS broker in the foreground (verbose).
  Run scripts\setup_mosquitto_tls.ps1 first to generate the certs/config.
  Keep this window open while you use the console / dashboard. Ctrl+C to stop.
#>
param([string]$MosqDir = "C:\Program Files\mosquitto")

$Root = Split-Path -Parent $PSScriptRoot
$conf = Join-Path $Root ".local\mqtt-certs\mosquitto-arcade.conf"
if (-not (Test-Path $conf)) {
    throw "Config not found: $conf`nRun scripts\setup_mosquitto_tls.ps1 first."
}
Write-Host "Starting Mosquitto TLS broker on :8883 (Ctrl+C to stop)..." -ForegroundColor Cyan
& "$MosqDir\mosquitto.exe" -c $conf -v
