@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\monitor.ps1" %*
pause
