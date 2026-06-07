# sm64coopdx VR

Super Mario 64 in VR, built on the sm64coopdx PC port. With a headset on, the game sits in front of
you as a small 3D diorama you can lean around and look into, with the sky wrapped around the room.
With no headset it just runs as the normal flatscreen game. Same exe, it works out which one you
want on its own.

Tested on Quest 3 and Pimax, but it should run with any PCVR / OpenXR runtime.

You bring your own Super Mario 64 US ROM. There's nothing from Nintendo in this repo, just code.
coopdx reads the rom locally when the game starts, same as normal sm64coopdx, and it never leaves
your machine.

## Download and play

1. Grab sm64coopdx-vr.zip from the Releases page.
2. Unzip it somewhere.
3. Put your SM64 US rom (a .z64 file) in the folder. On first run you can also just drag the rom
   onto the window.
4. Run sm64coopdx.exe.

If a headset is connected it boots into VR, otherwise you get the flat game. Start your VR runtime
first (Quest Link, Virtual Desktop, SteamVR) if you want VR. Your mods and saves carry over like
normal.

Already have sm64coopdx installed? You can skip the zip and just drop the release's sm64coopdx.exe
and libopenxr_loader.dll into your existing folder.

## VR controls

Keyboard keys for adjusting the view while you play. Everything else is the usual game controls.

| Key | What it does |
|-----|--------------|
| F10 | cycle view (diorama / close-up / first-person) |
| F3 / F4 | world closer / farther |
| F1 / F2 | world bigger / smaller |
| F5 / F6 | world down / up |
| F8 / F9 | less / more depth |
| F7 | reset and recenter |
| [ / ] | steadier / more head parallax |

If it ever drifts off, F7 puts it back in front of you.

## Building from source

Skip this if you just grabbed the release. To build it yourself:

1. Set up MSYS2 and the normal sm64coopdx build environment (see the upstream repo at
   https://github.com/coop-deluxe/sm64coopdx).
2. Put your baserom.us.z64 in the project root.
3. Run build_vr.bat. It pulls the one extra dependency (the OpenXR SDK) and compiles. You end up
   with build\us_pc\sm64coopdx.exe.

vr-support.patch is the whole VR change as one diff, if you'd rather apply it to a clean checkout
and build that. The technical writeup is in VR_README.md.

## Credits

sm64coopdx by the Coop Deluxe Team. Super Mario 64 belongs to Nintendo, bring your own rom. VR work
by RaYRoD.
