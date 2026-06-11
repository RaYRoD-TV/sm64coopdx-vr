# Changelog

## Unreleased

First person
- Talking to characters no longer freezes the game in first person. Toads,
  Bob-omb buddies, Koopa the Quick, cap switches and the other cutscene-driven
  conversations used to leave Mario stuck in place with no dialog box, because
  the camera system that creates those dialogs was asleep while first person
  held the camera. The dialog flow now runs in first person (VR and flatscreen)
  without moving your view: the text box appears, your answer registers, and
  the ease-back pulls back to show Mario talking. One conversation hanging also
  used to quietly break every later interaction in the level until you switched
  view modes - that leak is gone too, and signs now hold Mario in place while
  the text is up, like they do in third person.
- Slides ride at body height: belly and butt slides, slide kicks and the slide
  after a long jump drop the eye toward the ground the same smooth way crawling
  already does, instead of leaving the view at standing height while Mario
  scrapes along below. Getting knocked flat drops it too, and getting up brings
  it back.
- FP Flip Cam now covers falling, getting hurt and the jump slides. Knockbacks
  tip the view the way the hit threw Mario - backward hits look up as you land
  on your back, forward hits look down - and settle back to level. The dive
  (and, gentler, the long jump) leans into the head-first lunge. Like the flip
  jumps, these only move the view when the FP Flip Cam toggle is on.

## v0.5 - 2026-06-10

VR controllers
- You can now play with the VR controllers themselves. Quest 3 / Quest 2 /
  Quest Pro Touch controllers, Index, HP Reverb G2, Windows Mixed Reality
  and Vive wands all have layouts, with a basic fallback for other OpenXR
  controllers. No gamepad needed.
- Default layout: left stick moves, right stick is the camera, A jumps,
  B punches, left trigger crouches (Z), squeezing either grip grabs and
  throws objects, the left menu button pauses, and clicking the right
  stick cycles the VR mode.
- The grips only act when something grabbable is in reach or already in
  Mario's hands, so squeezing them in open space does not punch.
- Reset to Default in the VR menu now resets the settings of the view mode
  you are in, instead of switching you to another mode. Other modes keep
  their tweaks.
- On Quest 3 the buttons land on the hands they belong to: the native
  Touch Plus profile is used when the runtime offers it, instead of
  letting the runtime translate the older Touch layout (which could put
  buttons on the wrong controller).
- The VR layout is fixed and independent of your gamepad bindings. Before,
  a punch rebound onto a different gamepad button would quietly move punch
  onto the wrong VR controller and break grabbing.
- The camera stick's left/right direction matches what you expect in VR
  (the flat-gamepad direction read inverted there), and the analog look
  speed is reduced so turning is easier to control.
- Menus navigate with the stick and A/B, like a gamepad, and the triggers
  flip pages in the paginated menus.
- Rumble plays through both controllers, in short self-expiring pulses so
  it can never get stuck vibrating non stop (the game toggles the motor
  every frame like the N64 Rumble Pak, and a lost stop request used to
  leave a long buzz running with nothing to cancel it).
- Triggers and grips only register past 60% travel and release under 40%,
  so a finger resting on the trigger does not cause inputs.
- Taking the headset off releases all controller input, and a gamepad can
  stay connected at the same time.
- The console prints which controller profile each hand bound, which is
  the first thing to check if a controller misbehaves.

## v0.4 - 2026-06-09

VR
- Each VR mode now keeps its own distance, size, height, stereo depth and head
  motion values. Switching modes brings back what you set for that mode, and
  every mode's values are saved between launches.
- Settings changed right before quitting are no longer lost to the save delay.
- The default Diorama size is now 1376.
- The VR menu is paginated, so Reset to Default and Back are no longer cut off.
  Flip pages with the < > buttons or the L/R triggers.
- Reset to Default now also resets the HUD Size slider.
- Added a World Scale slider for first person: it changes how big the world
  feels on foot. Default is unchanged, and it only affects first person.
- Door and level transitions (the fade and wipe) now fill the whole view
  instead of showing in a small panel.
- Talking to a character no longer zooms the view in too far. The diorama
  framing holds while the text shows.
- Third-person mode no longer dips the camera through the floor on quick
  camera moves.

First person
- Optional Move Where You Look: Mario walks and turns toward where you are
  looking. Off by default, with a toggle in the VR menu.
- Sliding and diving no longer pull the camera back, so the ground does not
  seem to slip out from under you mid-slide.
- The view no longer gets stuck at a sign after reading it: the camera returns
  faster once the dialog closes, snaps back as soon as you move, and a rare
  sign-camera softlock now ends cleanly.

Menus
- The button press that opens a dialog no longer skips its first page.

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
- New VR Mode selector in the menu: Diorama, Third-person, First-person. You can
  also cycle the mode with d-pad up or the F10 key.
- Diorama mode now reads like an actual model on a table: smaller world, sat
  lower and closer, with stronger stereo depth and a slight downward tilt so you
  look down at it.
- Diorama and Third-person look controls now respond directly like first person
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
