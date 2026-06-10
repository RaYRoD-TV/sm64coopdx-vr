# sm64coopdx VR - notes

Technical notes for the VR build. If you just want to play, the main README covers that.

## What it does

VR runs through OpenXR. The scene is drawn twice per frame, once per eye, 
with real positional head tracking so you can lean in and look around it.

The sky is rebuilt as a 3D sphere drawn inside each eye, so it stays put when you turn your head
instead of sliding around like the original 2D skybox does. Menus and the other flat screens (title,
file select, pause, course/star select, dialogs) are shown on a panel in front of you rather than
smeared across the diorama.

At startup the exe checks OpenXR for a connected headset and turns VR on if it finds one. No headset
(or no OpenXR runtime) and it just runs as the normal flat game. You can force either way with --vr
or --novr. Built against OpenXR 1.0, tested mostly on Virtual Desktop's runtime (VDXR).

## Running it

Run sm64coopdx.exe. Start your VR runtime first (Quest Link, Air Link, Virtual Desktop, or SteamVR)
if you want VR. Put your own SM64 US ROM in the folder named baserom.us.z64 (or drag any .z64 onto the
window). There's no Nintendo data in the exe; it reads the rom at startup the same way normal coopdx does.

You can play with your VR motion controllers, or with the usual coopdx input: a gamepad (DualSense,
DS4, Xbox) or mouse and keyboard. Head tracking drives the view either way.

### VR controllers

The motion controllers are read through OpenXR actions and feed the game's pad directly with a
fixed layout: left stick moves, right stick is the camera (its left/right is flipped relative to a
flat gamepad, because in VR the world rotates around you rather than a camera panning on a screen,
and the analog look runs at reduced speed - VR_CAM_STICK_SCALE in controller_vr.c), A jumps, B
punches, left trigger is Z, either grip grabs and throws (it acts as B, which is SM64's grab
interaction), the left controller's menu button pauses, and clicking the right stick cycles the VR
mode (it acts as d-pad up). In menus the triggers flip pages.

The layout is deliberately NOT routed through the gamepad bindings. Gamepad binds carry personal
flat-screen habits (punch moved onto a different face button, for instance) and inheriting them
puts VR buttons on the wrong controller while looking exactly like a binding bug. The DJUI menus
navigate off the pad's A/B, so they follow the same fixed layout.

Triggers and grips register past 60% travel and release under 40%, so a resting finger can't
flicker an input. Rumble goes to both hands. Everything releases cleanly when the headset comes
off, and a gamepad can stay connected at the same time; inputs merge.

Bindings are suggested for these interaction profiles: Quest Touch, Quest 3 / Quest Pro Touch Plus
(used when the runtime offers it, so buttons land on the right hands without the runtime translating
the older Touch layout), Valve Index, HP Reverb G2, Windows Mixed Reality wands, Vive wands, and the
basic khr fallback. Anything else (Pimax included) goes through whichever of those layouts its
runtime emulates, or the runtime's own rebinding; SteamVR users can also remap everything in the
SteamVR controller bindings UI since the actions are exposed there with readable names. At boot and
on any controller change, the console prints which profile each hand actually bound:

    [VR] left controller profile: /interaction_profiles/...
    [VR] right controller profile: /interaction_profiles/...

If a controller misbehaves (wrong hand, dead buttons), include those two lines in the report; they
say what the runtime matched, which is where that class of bug lives.

### VR settings (in-game)

All tuning is in the in-game menu: pause and open VR (the button right after Mod Menu) to set the VR
mode, view distance, size, height, stereo depth, head motion, and camera anti-clip, with a Reset to
Default button. The settings span three pages; flip them with the < > buttons or the L/R triggers.
Each VR mode keeps its own values, so tweaks you make in Diorama come back when you return to it, and
everything is saved between launches. You can also cycle the VR mode (Diorama / Third-person /
First-person) with d-pad up or the F10 key.

## Building

Same as a normal sm64coopdx build (MSYS2 + MinGW), plus one extra package for OpenXR:

    pacman -S mingw-w64-x86_64-openxr-sdk

Then run build_vr.bat, or just `make` from an MSYS2 MINGW64 shell. build_vr.bat installs that package
for you if it's missing.

The exe itself is statically linked, but the OpenXR loader (libopenxr_loader.dll) is built with MinGW
and pulls in the MinGW runtime DLLs, so these four need to sit next to the exe: libopenxr_loader.dll,
libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll. discord_game_sdk.dll already ships with
coopdx. The build stages all of them in build/us_pc.

## Sharing

Neither way involves any game files:

- The Releases zip is the whole thing (exe + libopenxr_loader.dll + coopdx's data folders). Unzip,
  add a rom, run.
- vr-support.patch is the entire VR change as one diff, to apply on top of a clean sm64coopdx
  checkout and build yourself.

The person you share with brings their own rom either way.

## Where the VR code lives

- src/pc/vr/vr.c, vr.h - the OpenXR work: session setup, per-eye view/projection matrices, stereo,
  head-tracking damping, the sky-dome matrix, the menu panel, the geometry anti-clip, the F10 view
  cycle, frame submit, the startup headset check, and the motion-controller actions (bindings,
  per-frame sync, haptics).
- src/pc/controller/controller_vr.c - the VR controllers as a game controller: maps the OpenXR
  action state onto the pad and the menu input, sharing the SDL gamepad keyspace so the normal
  bindings and rebind UI apply.
- src/pc/gfx/gfx_pc.c, gfx_opengl.c - the per-eye / sky-dome / flat-panel render paths.
- src/game/skybox.c - builds the 3D sky sphere for VR.
- src/pc/pc_main.c - the VR frame loop and the auto-detect.
- src/pc/cliopts.c, cliopts.h - the --vr / --novr flags.
- Makefile - adds src/pc/vr and links the OpenXR loader.

## Still rough

It's beta. Motion controllers cover buttons, sticks, triggers, grips and rumble; there are no
tracked hands or pointer-based menu aiming yet, so menus navigate gamepad-style with the stick.
A few of the flat screens may want their panel distance nudged. Worth testing across different
levels and runtimes. A Theater mode (the flat game on a big
world-locked screen, with an optional image backdrop) exists in the code but is hidden until it's
finished - see VR_THEATER_ENABLED in src/pc/vr/vr.h.
