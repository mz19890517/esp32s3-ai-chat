param(
    [string]$Port = "COM20",
    [switch]$Clean
)

# Doubles as "flash.bat" target. Everything is ASCII on purpose:
# Windows PowerShell 5.1 mis-decodes UTF-8 without BOM, so keep it plain.

Set-Location -Path $PSScriptRoot

$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
if (-not (Test-Path $pio)) {
    Write-Host ""
    Write-Host "[ERROR] platformio.exe not found:" -ForegroundColor Red
    Write-Host "        $pio" -ForegroundColor Red
    Write-Host "        install with:  pip install -U platformio" -ForegroundColor Yellow
    Read-Host "Press Enter to exit"
    exit 1
}

if ($Clean) {
    Write-Host "Cleaning .pio\build\esp32s3 ..." -ForegroundColor Cyan
    Remove-Item -Recurse -Force ".pio\build\esp32s3" -ErrorAction Ignore
}

Write-Host ""
Write-Host "=== Serial ports ===" -ForegroundColor Cyan
& $pio device list

$log = Join-Path $PSScriptRoot "build_new.log"
Write-Host ""
Write-Host "=== Build + upload to $Port  (full rebuild takes 2-5 min) ===" -ForegroundColor Cyan
& $pio run -t upload --upload-port $Port 2>&1 | Tee-Object -FilePath $log

$verified = Select-String -Path $log -Pattern "Hash of data verified" -Quiet
$failed = Select-String -Path $log -Pattern "error:|Failed|No serial data received|could not open port" -Quiet

Write-Host ""
if ($verified) {
    Write-Host "=== FLASH OK. Board rebooted. Firmware version should read 1.3.0 ===" -ForegroundColor Green
    Write-Host "    (Settings -> Firmware / OTA)" -ForegroundColor Green
} elseif ($failed) {
    Write-Host "=== FLASH FAILED. See build_new.log ===" -ForegroundColor Red
    Write-Host "    Stuck at Connecting: hold BOOT, tap RST, release BOOT, then rerun." -ForegroundColor Yellow
    Write-Host "    Wrong port: pick one from the 'Serial ports' list above." -ForegroundColor Yellow
} else {
    Write-Host "=== Finished, but no 'Hash of data verified' in log - check build_new.log ===" -ForegroundColor Yellow
}
Read-Host "Press Enter to exit"
