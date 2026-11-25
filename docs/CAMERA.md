# The camera (US program, SLUS-21501)

What is known of Raw Danger's camera, from the work on the orbit camera and the fixed cameras (v0.9.0). Addresses are the US
program's; the Japanese one has the same code elsewhere.

## Camera blocks and modes

- One block of 0x490 bytes per camera mode: `0x5FD7D0 + mode * 0x490`. The active block's address is in the word at
  `0x48F4E0` (gp - 0x4990); its mode at block + 0x3EC.
- `0x17F610` switches the mode (a0 the mode, f12 the blend time): it copies the old camera's place and axes into the new block
  and makes it the active one. `0x17FF30` sets every block up at the start of a map (it calls 0x17F610 for modes 0 to 21).
- `0x17C250` runs the active camera once a frame (a jump table at 0x457660 on the mode); it is called from `0x17C9B0`.
- Modes with a parameter block (0x3A8D40 + mode * 80): 0 (the follow camera), 1 (the first-person view), 16, 17, 19 (wider
  follow cameras). Mode 10 is a camera whose place the map's data or a cutscene sets: the cutscenes' shots (0x17A340, called from
  0x17A4EC) and, the user reports, the fixed views of some places. Mode 9 and 13 are set by 0x158640 (a map's start).

## The follow camera (mode 0)

- `0x177410` -> `0x176F70`: the right stick. a1 = 1 / 0 axes read with 0x2DAB80; the turn (f22) is -stick x times
  (camera + 0x2C) degrees per step, plus the mouse's turn at 0x7F10 ([Direct mouse look]), turned round the pivot (+0x310) with
  sin / cos (0x2D7B70, 0x2D7D00). The up / down step is f20 = stick y * 0.2 when +0x3C4 is set.
- The height step (0x177270 to 0x177348): +0x35C (the "height", limited by +0x14 and +0x18: 2.5 and 6.5), the nearest and the
  farthest distance along the ground (+0x360, +0x364: 5 and 6) all grow by the step, the camera slides back along the ground by
  it, and the look-at offset (+0x3B8) grows by 1.2 times it. So up / down is a zoom as much as a tilt.
- `0x177440` the follow itself: the camera is put `BASE + LOOK` over the pivot (y points down: camera y = pivot y - (+0x354 +
  +0x3B8) + +0x358), with +0x354 = +0x10 (0.4) times +0x370; that height is stopped at -0.4 under the pivot (0x17754C, moved to
  -12 by [Free camera tilt]); +0x358 keeps it under ceilings. Along the ground it stays between +0x360 and +0x364 (times +0x368).
- Places: +0x160 the camera (the one the picture is drawn from, also at +0x90 in the view matrix), +0x310 the pivot (the follow
  point, 1.5 over the hero's feet), +0x260 to +0x2A0 the camera at the last mode switch, +0x450 the zoom factor (the field of
  view: the screen distance is 768 times it), +0x448 non-zero while the game animates a zoom.

## What the PC version adds

- Orbit camera (v0.9.0, `[Orbit camera]`, engine host call 200 at 0x177270, `PS2_CAMERA_ORBIT`): the step turns the camera round
  the pivot by writing the follow's own words (height over the pivot through +0x3B8, both ground distances), keeping the distance.
- Fixed cameras off (v0.9.0, `[Free camera everywhere]`, host call 201 at 0x17C9B0, `PS2_FREE_CAMERA`): with the setting on, mode
  10 while playing (scene word 0x48F488 = 21) and with no cutscene bars drawn for a second goes back to mode 0 through 0x17F610.
  Checked: the tutorial's cutscenes keep their shots. Not checked yet: a place with a fixed camera while playing (none was at hand
  in the saved states).
- Field of view (+0x450), free tilt (the -0.4 stops), direct mouse look (0x7F10): v0.8.x.
