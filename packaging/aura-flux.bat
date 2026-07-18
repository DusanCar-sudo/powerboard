@echo off
REM Aura-FLUX Windows Launcher
REM Opens in new terminal window

start "Aura-FLUX" cmd /K "cd /D %~dp0 && powerboard.exe"
