@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo.
echo   ESP32-S3 蓝牙 OTA 网页
echo   页面地址: http://localhost:8080/
echo   用 Chrome / Edge 打开，按 Ctrl+C 结束。
echo.
start "" http://localhost:8080/
"%USERPROFILE%\.platformio\penv\Scripts\python.exe" -m http.server 8080
