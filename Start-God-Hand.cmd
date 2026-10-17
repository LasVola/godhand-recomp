@echo off
rem Starts God Hand (PC recomp) in a window. Put your own God Hand (USA) disc image in game\ (or set GODHAND_ISO).
rem Log of the last session: build\port\bin\logs\play_last.log
cd /d "%~dp0"
set GODHAND_NORMAL_PRIORITY=1
start "God Hand log" /min cmd /c "python -I build.py run > build\port\bin\logs\play_last.log 2>&1"
