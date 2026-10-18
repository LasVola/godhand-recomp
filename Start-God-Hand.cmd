@echo off
rem Starts God Hand (PC recomp) in a window. Put your own God Hand (USA) disc image in game\ (or set GODHAND_ISO).
rem The minimized "God Hand log" window only collects the log; it closes by itself with the game.
rem Log of the last session: build\port\bin\logs\play_last.log (the one before is kept as logs\play_<time>.log)
cd /d "%~dp0"
set PS2X_AUDIO_STATS=1
set PS2X_PACING_STATS=1
python -I build.py keep-play-log
start "God Hand log" /min cmd /c "python -I build.py run > build\port\bin\logs\play_last.log 2>&1"
