# sm64coopdx VR

Super Mario 64 in VR, built on the sm64coopdx PC port. With a headset on, the game renders in immersive VR. You can lean around and look into the world.
With no headset it just runs as the normal flatscreen game. Same exe, it works out which one you
want on its own.

Tested on Quest 3 and Pimax Dream Air, but it should run with any PCVR / OpenXR runtime.

You bring your own Super Mario 64 US ROM. There's nothing from Nintendo in this repo, just code.
coopdx reads the rom locally when the game starts, same as normal sm64coopdx, and it never leaves
your machine.

## Download and play

1. Grab sm64coopdx-vr.zip from the Releases page.
2. Unzip it somewhere.
3. Put your Super Mario 64 US ROM in the folder and name it baserom.us.z64. (On first run you can
   also just drag any .z64 onto the window and it sets that up for you.)
4. Run sm64coopdx.exe.

If a headset is connected it boots into VR, otherwise you get the flat game. Start your VR runtime
first (Quest Link, Virtual Desktop, SteamVR) if you want VR. Your mods and saves carry over like
normal.

Already have sm64coopdx installed? You can skip the unzip step and just copy the exe plus all the
DLLs from the release (libopenxr_loader.dll and the libgcc/libstdc++/libwinpthread ones next to it)
into your existing folder.

## Playing solo (offline)

Just click **Play** on the main menu and you're straight into the game, on your own, fully offline, with
no server screens. Underneath it's a normal Direct Connection host with the defaults, so co-op still
works too: someone can join your IP, or use **Host** to set up a session with the usual options.

## Controls

You play with any of these:

- Your VR controllers. Quest 3 / Quest 2 / Quest Pro Touch controllers, Index, HP Reverb G2,
  Windows Mixed Reality and Vive wands all have layouts wired up, and other OpenXR controllers get
  the runtime's own remapping on top of those. No gamepad needed. See the layout below.
- Gamepad: DualSense (PS5), DualShock 4 (PS4), Xbox, Switch Pro, or any other SDL-compatible
  controller, wired or over Bluetooth. It uses your existing coopdx control bindings.
- Mouse and keyboard.

In VR your head moves the view on top of whichever input you use.

### VR controller layout

The VR controllers show up to the game as a regular gamepad, so the default layout below comes from
the normal coopdx bindings and you can change it in Options like any controller:

| Control | Does |
| --- | --- |
| Left stick | Move |
| Right stick | Camera (C buttons) |
| A | Jump (A) |
| B | Punch (B) |
| Left trigger | Crouch / ground pound (Z) |
| Right trigger | R |
| Grips | Grab and throw objects (acts as B) |
| Menu button (left controller) | Pause (Start) |
| Right stick click | Cycle the VR mode |
| X / Y | X / Y |

Menus work with it too: move the cursor with a stick, A to select, B to go back. Rumble plays
through the controllers. The Oculus button on the right controller belongs to the system and can't
be bound.

### VR menu

All the VR settings are in-game. Pause and open the VR button, right after Cheats:

![VR button in the pause menu](VR_Menu_Preview/image1.png)

It has the VR mode, diorama distance, size and height, menu and HUD size, stereo depth, head motion,
the first person toggles, Hide HUD, and camera anti-clip, plus a Reset to Default button:

![VR settings panel](VR_Menu_Preview/image2.png)

You can cycle the VR mode (Diorama / Third-person / First-person) with d-pad up or the F10 key, or
pick it from the VR Mode dropdown in the menu. Each mode remembers the settings you give it, and
everything in the menu is saved between launches.

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
