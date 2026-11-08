@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\flash.ps1" %*
pause
