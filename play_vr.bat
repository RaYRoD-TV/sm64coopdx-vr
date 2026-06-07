@echo off
title sm64coopdx VR
REM ===========================================================================
REM  Launch sm64coopdx in OpenXR VR mode (Step 0 pipeline proof).
REM
REM  BEFORE running, make sure a PCVR / OpenXR runtime is live:
REM    Quest 3      -> Quest Link (cable) / Air Link / Virtual Desktop
REM                    (set Oculus or SteamVR as the active OpenXR runtime)
REM    Pimax Dream Air -> Pimax Play  (or SteamVR)
REM
REM  --console keeps this window open with the [VR] boot trace.
REM  If VR can't engage, the game still runs flat on the desktop.
REM ===========================================================================

cd /d "%~dp0build\us_pc"

if not exist "sm64coopdx.exe" (
    echo ERROR: sm64coopdx.exe not found in build\us_pc.
    echo Build the game first ^(make -j in the MSYS2 MINGW64 shell^).
    pause
    exit /b 1
)

echo Launching sm64coopdx in VR mode...
echo.
sm64coopdx.exe --vr --console

echo.
echo === Game exited. Scroll up to review any [VR] lines. ===
pause
