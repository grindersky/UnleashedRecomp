@echo off
rem Opens the TAS Mods and Codes window (mods and codes for the Linux/WSL build).
start "" powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0TasModsAndCodes.ps1"
