@echo off
rem Double-click to build and flash. Optional args:  flash.cmd COM21  |  flash.cmd -Clean
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" %*
