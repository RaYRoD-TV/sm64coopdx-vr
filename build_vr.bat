@echo off
title Build sm64coopdx (VR)
REM ===========================================================================
REM  One-click build for the VR-enabled sm64coopdx.
REM
REM  Prerequisites:
REM    1. MSYS2 + the MinGW-w64 toolchain (the normal sm64coopdx build env).
REM    2. Your OWN legally-obtained US SM64 ROM as baserom.us.z64 in this folder
REM       (used only for local asset extraction; it is NEVER distributed).
REM  The one extra dependency (the OpenXR SDK) is installed automatically below
REM  if it is missing.
REM
REM  CHERE_INVOKING=1 keeps the MSYS login shell in THIS folder so make finds the
REM  Makefile. OS=Windows_NT is inherited from cmd, so the Makefile detects a
REM  Windows build (otherwise it tries a Linux link and fails on -rdynamic).
REM ===========================================================================

cd /d "%~dp0"
set "MSYSTEM=MINGW64"
set "CHERE_INVOKING=1"
set "BASH=C:\msys64\usr\bin\bash.exe"

if not exist "%BASH%" (
    echo ERROR: MSYS2 bash not found at "%BASH%".
    echo Install MSYS2 from https://www.msys2.org and set up the sm64coopdx
    echo build environment first - see VR_README.md.
    pause
    exit /b 1
)

echo Checking the OpenXR SDK dependency, then building...
echo.
"%BASH%" -lc "if [ ! -f /mingw64/include/openxr/openxr.h ]; then echo '[deps] OpenXR SDK missing - installing via pacman...'; pacman -S --needed mingw-w64-x86_64-openxr-sdk || { echo '[deps] If pacman said target not found, run: pacman -Sy   then re-run this script.'; exit 1; }; fi; make -j"

if errorlevel 1 (
    echo.
    echo === BUILD FAILED - scroll up for the error. ===
    echo  - pacman 'target not found': run 'pacman -Sy' in MSYS2, then retry.
    echo  - 'Permission denied' on ld.exe: the game is still running - close it.
    echo  - missing baserom: put your own baserom.us.z64 in this folder.
    pause
    exit /b 1
)

echo.
echo === Build complete: build\us_pc\sm64coopdx.exe ===
echo Run play_vr.bat to launch in VR.
pause
