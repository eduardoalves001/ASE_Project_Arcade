<#
.SYNOPSIS
  Builds and flashes ESP-Arcade to the ESP32-C6, then opens the serial monitor.

.EXAMPLE
  .\scripts\flash-arcade.ps1                 # uses COM3
  .\scripts\flash-arcade.ps1 -Port COM5      # other port
  .\scripts\flash-arcade.ps1 -NoMonitor      # flash only, skip the monitor

  To leave the serial monitor: press  Ctrl + ]
#>
param(
    [string]$Port = "COM3",
    [switch]$NoMonitor
)

# 1. Load the ESP-IDF 5.5 environment (space-free install path)
$env:IDF_TOOLS_PATH = "C:\esp\tools"
& "C:\esp\esp-idf\export.ps1"

# 2. Move into the project (via the space-free junction to the Desktop project)
Set-Location "C:\esp\arcade"

# 3. Build + flash (+ monitor)
if ($NoMonitor) {
    idf.py -p $Port flash
} else {
    idf.py -p $Port flash monitor
}
