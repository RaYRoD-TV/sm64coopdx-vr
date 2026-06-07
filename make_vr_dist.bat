@echo off
title Package sm64coopdx VR (complete download)
REM ===========================================================================
REM  Builds the COMPLETE standalone VR release: "unzip, add your ROM, run".
REM
REM  Contents: the VR sm64coopdx.exe + the two needed DLLs (libopenxr_loader.dll,
REM  discord_game_sdk.dll) + coopdx's runtime data folders (mods, lang, dynos,
REM  palettes) + a readme. NO .bat files, NO updater (so it can't replace the VR
REM  exe), and NO Nintendo files - the player adds their OWN .z64 ROM at runtime.
REM
REM  Shareable: the exe contains no Nintendo assets (coopdx loads them from the
REM  player's own ROM at runtime), exactly like the official sm64coopdx download.
REM ===========================================================================

cd /d "%~dp0"
set "SRC=build\us_pc"
set "DIST=dist\sm64coopdx-vr"
set "ZIP=dist\sm64coopdx-vr.zip"

if not exist "%SRC%\sm64coopdx.exe" (
    echo ERROR: %SRC%\sm64coopdx.exe not found. Build it first ^(build_vr.bat^).
    pause
    exit /b 1
)

echo Cleaning %DIST% ...
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"

echo Copying exe + DLLs ...
copy /y "%SRC%\sm64coopdx.exe"        "%DIST%\" >nul
copy /y "%SRC%\libopenxr_loader.dll"  "%DIST%\" >nul
copy /y "%SRC%\discord_game_sdk.dll"  "%DIST%\" >nul
copy /y "VR_README.md"                "%DIST%\" >nul

echo Copying coopdx runtime data ^(mods, lang, dynos, palettes^) ...
robocopy "%SRC%\mods"     "%DIST%\mods"     /E /NFL /NDL /NJH /NJS /NC /NS /NP >nul
robocopy "%SRC%\lang"     "%DIST%\lang"     /E /NFL /NDL /NJH /NJS /NC /NS /NP >nul
robocopy "%SRC%\dynos"    "%DIST%\dynos"    /E /NFL /NDL /NJH /NJS /NC /NS /NP >nul
robocopy "%SRC%\palettes" "%DIST%\palettes" /E /NFL /NDL /NJH /NJS /NC /NS /NP >nul
if errorlevel 8 ( echo ERROR: a data folder failed to copy. & pause & exit /b 1 )

echo Writing READ_ME_FIRST.txt ...
> "%DIST%\READ_ME_FIRST.txt" echo sm64coopdx VR - complete edition
>>"%DIST%\READ_ME_FIRST.txt" echo.
>>"%DIST%\READ_ME_FIRST.txt" echo HOW TO PLAY:
>>"%DIST%\READ_ME_FIRST.txt" echo  1. Put your own Super Mario 64 US ROM (a .z64 file) into THIS folder.
>>"%DIST%\READ_ME_FIRST.txt" echo     (Or drag it onto the window the first time you run the game.)
>>"%DIST%\READ_ME_FIRST.txt" echo  2. If you have a VR headset: start your PCVR / OpenXR runtime first
>>"%DIST%\READ_ME_FIRST.txt" echo     (Quest Link, Virtual Desktop, or SteamVR).
>>"%DIST%\READ_ME_FIRST.txt" echo  3. Run sm64coopdx.exe.
>>"%DIST%\READ_ME_FIRST.txt" echo.
>>"%DIST%\READ_ME_FIRST.txt" echo That is it. Headset connected = VR automatically; no headset = normal flat
>>"%DIST%\READ_ME_FIRST.txt" echo game. Force it either way with  --vr  or  --novr .
>>"%DIST%\READ_ME_FIRST.txt" echo.
>>"%DIST%\READ_ME_FIRST.txt" echo VR keys: F10 cycle view, F3/F4 distance, F1/F2 size, F7 recenter.
>>"%DIST%\READ_ME_FIRST.txt" echo See VR_README.md for the full guide.
>>"%DIST%\READ_ME_FIRST.txt" echo.
>>"%DIST%\READ_ME_FIRST.txt" echo This download contains NO Nintendo files. coopdx reads Super Mario 64's
>>"%DIST%\READ_ME_FIRST.txt" echo textures and audio from YOUR OWN ROM at runtime - you must own the game.

echo Zipping ...
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%DIST%\*' -DestinationPath '%ZIP%' -Force"

echo.
echo === Done. ===
echo   Folder: %DIST%
echo   Zip:    %ZIP%
echo   This is the complete download: unzip, add a .z64 ROM, run sm64coopdx.exe.
pause
