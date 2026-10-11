@echo off
rem Quick start: checks tools, finds your floppy images, installs, lifts,
rem builds, makes a shortcut. See README "Getting Started". Safe to run again;
rem it resumes.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1" %*
pause
