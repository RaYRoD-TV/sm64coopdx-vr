# Changelog

## v0.3 - 2026-06-08

VR
- Renamed the "Tabletop" mode to "Diorama".
- Diorama mode now uses the free orbit camera, so you can look around the model
  from any angle. It has smooth wall collision that eases the camera in front of
  a wall between you and the model and ignores floors and ceilings, so tilting
  the view up or down no longer yanks the camera around.
- The VR settings menu now keeps the live diorama in view while it is open, so
  you can see distance, size, height and stereo changes as you make them.
- VR settings are now remembered between launches: mode, distances, sizes,
  stereo depth, head motion, HUD size, anti-clip, and the first-person toggles.
- Added a HUD Size slider to scale the in-headset HUD.
- Frames are now paced by the headset itself instead of the desktop monitor's
  refresh, which helps smoothness when the two run at different rates.

First person
- The flip cam now rolls the side flip toward the side it actually flips, and
  tree jumps flip too.
- You can look left and right while hanging on a ledge instead of being locked
  facing the wall.
- In Diorama mode the C-up look-around no longer engages, so it cannot freeze
  your movement.

Hide HUD
- Hide HUD now hides the gameplay HUD, including mod-drawn HUDs, while leaving
  menus and pause screens visible.

Menus
- Added a Mods button to the main menu, so you can pick your mods before
  pressing Play. Mods load when the game starts.

## v0.2 - 2026-06-08

VR
- New VR Mode selector in the menu: Diorama, Close-up, First-person. You can
  also cycle the mode with d-pad up or the F10 key.
- Diorama mode now reads like an actual model on a table: smaller world, sat
  lower and closer, with stronger stereo depth and a slight downward tilt so you
  look down at it.
- Diorama and Close-up look controls now respond directly like first person
  instead of the laggy accelerated camera.
- The view recenters on whichever way you're facing when the game opens, so the
  world is in front of you no matter how you started.
- The menu panel sits at eye level instead of drifting toward the bottom.
- Mouse look no longer inverts itself when the headset takes or loses focus.
- Added a Hide HUD toggle in the VR menu (works in flatscreen too).

First person
- Flip cam follows the actual flip in both flatscreen and VR: forward and back
  somersaults (triple jump, slide flip, backflip, rollouts) pitch the view, and
  the side flip rolls it to the side. Smoothed so it isn't choppy.
- First person stays on while flying with the wing cap.
- Added ease-back: when you talk to something or attack, the camera pulls back
  to show Mario, then eases back into first person. He stays hidden otherwise
  and is solid, not see-through, while shown.
- Removed the old Show Body option. The ease-back replaces it.

Input
- Gamepad reliably captures menus now, including the in-game pause menu, even
  when the mouse last had the cursor.

Flatscreen
- Skybox covers the full screen again with no black bars, and stays anchored
  without swimming, including after taking the headset off.
