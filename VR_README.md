# sm64coopdx VR - notes

Technical notes for the VR build. If you just want to play, the main README covers that.

## What it does

VR runs through OpenXR. The scene is drawn twice per frame, once per eye, as a small 3D diorama
floating in front of you, with real positional head tracking so you can lean in and look around it.
The sky is rebuilt as a 3D sphere drawn inside each eye, so it stays put when you turn your head
instead of sliding around like the original 2D skybox does. Menus and the other flat screens (title,
file select, pause, course/star select, dialogs) are shown on a panel in front of you rather than
smeared across the diorama.

At startup the exe checks OpenXR for a connected headset and turns VR on if it finds one. No headset
(or no OpenXR runtime) and it just runs as the normal flat game. You can force either way with --vr
or --novr. Built against OpenXR 1.0, tested mostly on Virtual Desktop's runtime (VDXR).

## Running it

Run sm64coopdx.exe. Start your VR runtime first (Quest Link, Air Link, Virtual Desktop, or SteamVR)
if you want VR. You need your own SM64 US rom (.z64) in the folder or dragged onto the window. There's
no Nintendo data in the exe; it reads the rom at startup the same way normal coopdx does.

### VR tuning keys (keyboard, while playing)

| Key | What |
|-----|------|
| F1 / F2 | world bigger / smaller |
| F3 / F4 | world closer / farther |
| F5 / F6 | world down / up |
| F7 | reset and recenter |
| F8 / F9 | less / more stereo depth |
| F10 | cycle preset (diorama / close-up / first-person) |
| [ / ] | steadier / more head parallax |

Whatever you tune gets saved to vr_tune.txt next to the exe.

## Building

Same as a normal sm64coopdx build (MSYS2 + MinGW), plus one extra package for OpenXR:

    pacman -S mingw-w64-x86_64-openxr-sdk

Then run build_vr.bat, or just `make` from an MSYS2 MINGW64 shell. build_vr.bat installs that package
for you if it's missing.

The MinGW runtime is linked statically, so the only extra DLL the exe needs over a stock coopdx is
libopenxr_loader.dll. discord_game_sdk.dll already ships with coopdx.

## Sharing

Neither way involves any game files:

- The Releases zip is the whole thing (exe + libopenxr_loader.dll + coopdx's data folders). Unzip,
  add a rom, run.
- vr-support.patch is the entire VR change as one diff, to apply on top of a clean sm64coopdx
  checkout and build yourself.

The person you share with brings their own rom either way.

## Where the VR code lives

- src/pc/vr/vr.c, vr.h - the OpenXR work: session setup, per-eye view/projection matrices, stereo,
  head-tracking damping, the sky-dome matrix, the menu panel, tuning keys, frame submit, and the
  startup headset check.
- src/pc/gfx/gfx_pc.c, gfx_opengl.c - the per-eye / sky-dome / flat-panel render paths.
- src/game/skybox.c - builds the 3D sky sphere for VR.
- src/pc/pc_main.c - the VR frame loop and the auto-detect.
- src/pc/cliopts.c, cliopts.h - the --vr / --novr flags.
- Makefile - adds src/pc/vr and links the OpenXR loader.

## Still rough

It's beta. Controller input isn't wired up yet, so it's head tracking plus the keyboard tuning keys
for now. A few of the flat screens may want their panel distance nudged. Worth testing across
different levels and runtimes.
