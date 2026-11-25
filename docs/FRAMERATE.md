# Frame rate (US, SLUS-21501): what sets the 20 steps a second

Found in v0.9.7 while putting the uncapped frame rate next to the animation work. Addresses are the US program's.

## The pacing

- The graphics block G is at `*(gp - 0x3E70)` (gp = 0x493E70, so the word 0x490000; G = 0x7007F0 here).
- The vblank interrupt (0x2DA670, installed through 0x2A61D8 by 0x2DA630) runs up to four callbacks from a table at 0x6C3060;
  the first, 0x2CBBA0, counts vblanks: G+0x23C (all of them) and G+0x254 (since the last picture), and flips when one is due.
- **G+0x24C is the number of vblanks a step waits for: 3 on the US disc** (20 steps a second); 0x2CBD10 sets it (a0), 0x2CBD00
  copies it from G+0x250. The waits are at 0x2CBA10, 0x2CBC64, 0x2CBC8C, 0x2CBCBC and 0x2CBE84 (`G+0x254 < G+0x24C`).

## What changes with it

Setting G+0x24C to 2 or 1 makes the game step 30 or 60 times a second, but it is a fixed-step game: holding forward for 120
vblanks from the same place in the hotel moved Joshua 3.6 m at 3, 5.4 m at 2 and 9.2 m at 1 (1.5 and 2.6 times as far). Every
speed, every timer and the animation are counted in steps.

The game was written for a frame rate fixed when it was compiled: the European release targets 30 (and runs at 25 on PAL TVs),
the US and Japanese ones 20. The value is folded into constants all over the program (88 places load the float 20.0 alone; many
more hold numbers worked out from it, such as speeds per step), and the scripts' sleeps are counted in steps. Souzooka's 2019
"Raw Danger! 60 FPS project" (an IPS patch for the US program, plus overlay changes for the menus) did this by hand.

## Ways to more frames

1. **More steps** (the game at 30 or 60): G+0x24C, plus every constant that holds the frame rate. The most reliable list would
   come from comparing the US program with the European one (the same code compiled for 30): every word that differs only by
   the frame rate is one of them. Then 60 is the same list again. Needs the European disc's program (SLES).
2. **In-between pictures** (the game at 20, the picture at 60): the camera and every model drawn again between two steps, with
   the places and the bones halfway (the engine already sees each bone's build and the world-to-screen matrix, 0x5FD690). Needs
   the draw task run again without the game's step, or the native renderer's own draws (docs/NATIVE_RENDERER.md).

## v0.9.9: the European program compared

The European disc (SLES_545.87, VER 1.02, PAL) has the same 6207 functions in the same order as the US program; 6198 have the
same length. Compared word by word (`rd.py framerate US_ELF EU_ELF 30`, tools/rd_framerate.py):

- **About 1150 constants change with the frame rate**: about 400 in the code (lui/ori floats such as 20.0 -> 30.0 and 0.05 ->
  0.0333, li counts such as the vblanks a step 3 -> 2, slti bounds) and about 750 floats in the data tables (per-step
  amounts like 3 -> 2, 18 -> 12, 0.18 -> 0.12). Most change by x1.5 or x2/3; accelerations by x4/9; some counts are rounded
  (5 -> 8, 6 -> 8, 7 -> 10); some angles per step were tuned for the PAL TV's 25 (pi/16 -> pi/20).
- Left out after a look at each: a sound number (0x209670), a table's size (0x24B68C), a place (0x348728), a bit mask
  (0x144FDC) and a value set with a screen size (0x19C390/98).
- The 79 code overlays (map scripts, menus) differ too, nearly all by the moved addresses of the main program's functions;
  about 120 small counts in them look like frame-rate ones (not applied yet).
- The data files are the same apart from the menus' pictures (IMENU), BOOT.DAT and one map file.

**Tested**: the US program with the European constants (patches/framerate/SLUS-21501_30steps.pnach, translated again) steps 30
times a second; after a fresh map load (the map select, the hotel), holding forward for 2 s moves Joshua 3.60 m, as the
original at 20. (From a saved state the old values copied into the characters when the map loaded stay: 5.40 m.)

**60 (extrapolated)**: the same changes applied three times instead of one and a half (x3, x1/3, x1/9, per-step factors
cubed-rooted): 2.85 m in 2 s, so not right yet; next, the walk at 20, 30 and 60 sampled step by step to find which
constants do not follow the law, and the overlays' counts.

**Not offered yet** in RawDangerPC.exe: the cutscenes, the scripts (the overlays), the menus at 30 and the PC version's own
features (the orbit camera's turn per step, the mouse look) still need checking at 30.

## v0.10.0: 60 works, with the EE overclocked

The first 60 table walked 2.85 m in 2 s. The reason was not the table: counting the pictures showed the game made only about 48
steps a second, because a step's work no longer fitted in one vblank of the emulated EE (the engine charges one cycle an
instruction; PCSX2's 60 fps patches need its EE cycle rate raised for the same reason). With PS2_EE_OVERCLOCK=2 (engine patch
0044) the same table walks 3.60 m, as at 20 and 30. The table (patches/framerate/SLUS-21501_60steps.pnach) now snaps each 20 -> 30
change to its law first (x1.5^n with n = +-1 or +-2, or x1.25^n for the few tuned to the PAL TV's 25), so a rounded count
(5 -> 8) becomes 15 at 60, not 18.

Still to do before it is an option: the overlays' counts, cutscenes, menus, and the PC version's own per-step features.

