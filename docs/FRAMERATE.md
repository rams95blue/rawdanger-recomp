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
