@echo off
rem Start the Prince of Persia recomp. Ctrl+N = new game, arrows + Shift to play.
cd /d "%~dp0"
build\pop1.exe
if errorlevel 1 pause
