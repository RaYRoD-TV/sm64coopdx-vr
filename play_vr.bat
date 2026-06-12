@echo off
title sm64coopdx VR
REM ===========================================================================
REM  Launches sm64coopdx with a console window so you can watch the VR boot log.
REM  --vr forces VR on; with a headset connected you can also just run the exe
REM  directly and it picks VR on its own.
REM
REM  Start your PCVR / OpenXR runtime first (Quest Link, Air Link, Virtual
REM  Desktop, or SteamVR). If VR can't start, the game runs flat instead.
REM ===========================================================================

cd /d "%~dp0build\us_pc"

if not exist "sm64coopdx.exe" (
    echo ERROR: sm64coopdx.exe not found in build\us_pc.
    echo Build the game first ^(build_vr.bat, or make from the MSYS2 MINGW64 shell^).
    pause
    exit /b 1
)

echo Launching sm64coopdx...
echo.
sm64coopdx.exe --vr --console

echo.
echo === Game exited. Scroll up to review any [VR] lines. ===
pause
