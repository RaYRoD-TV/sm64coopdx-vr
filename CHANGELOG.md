# Changelog

## v0.2 - 2026-06-08

VR
- New VR Mode selector in the menu: Tabletop, Close-up, First-person. You can
  also cycle the mode with d-pad up or the F10 key.
- Tabletop mode now reads like an actual model on a table: smaller world, sat
  lower and closer, with stronger stereo depth and a slight downward tilt so you
  look down at it.
- Tabletop and Close-up look controls now respond directly like first person
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
