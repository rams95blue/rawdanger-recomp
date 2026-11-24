# Native renderer: plan (v0.8.5)

The game's code runs natively, but its pictures are still made by reproducing the PS2's graphics chain one step at a time:
the game builds packets for the vector unit VU1, a VU1 microprogram transforms and lights the vertices and sends triangles to the
GS, and the engine reproduces the GS on the GPU (`runtime/src/gs/gpu`). Everything a PC port would do better, water, lights,
shadows, frame rate, sits on the far side of that chain, after the game's scene has already been reduced to PS2 triangles.

A native renderer takes the scene one step earlier, from what the game hands VU1, and draws it with a modern pipeline. The 2D
(HUD, menus, subtitles), the movies and the game's colour filter stay on the GS path, which already works.

## What is known

- The game's frame: 20 game steps a second (the graphics library's vblanks-per-frame is set to 3 at 0x16D120, 0x16D470, 0x16DB10;
  1 at 0x212F8C for another mode). Game logic does not read the vblank counters: every step is 1/20 s of game time.
- The 3D goes through VU1 (`PS2_VU_LIVE`), the colour filter runs after the 3D and before the HUD (docs/GRAPHICS.md), the depth
  buffer is Z32 at 0x2300, about 1 / distance.
- The camera block (0x5FD7D0, a fixed address; the word at 0x48F7D8 is not a lasting pointer to it): position +0x160, target
  (the player) +0x310, view direction +0x1B0 (y points down). The follow camera tilts by moving its height behind the hero:
  +0x35C, limited by +0x14 (2.5, looking up) and +0x18 (6.5, looking down). +0x30 / +0x34 (-80 and 0 degrees) limit another view.

## Phases

1. **Look**: name the VU1 microprograms by the hash of their code, count their kicks per frame and per map, and dump one frame's
   VU1 input (the VIF unpacks: vertices, normals, texture coordinates, matrices, light settings). Output: a table of the
   microprograms that draw the world, the characters, the water and the effects.
2. **Characters and the world, natively**: for those microprograms, read the same input the microprogram reads and draw it on the
   GPU directly: float depth, any resolution, MSAA, no PS2 limits on draw calls. The GS path keeps drawing everything else.
   Checked frame by frame against the GS picture.
3. **Frame rate**: with the scene's transforms in hand, keep each object's last two game steps and draw the frames in between
   (interpolation, as PC ports of fixed-step games do): 60 frames a second or more on screen while the game still steps 20
   times a second at its own speed. This is how item 9 of the pending list gets done without changing the game's timings.
4. **Light**: the game's lights as real per-pixel lights, shadow maps for the main lights, the ambient occlusion moved here.
5. **Water**: the flood's surfaces drawn with moving normals, reflections (screen space, then planar for large calm water) and
   depth-based colour.

Each phase ships on its own, behind an option, with the GS picture as the reference and the fallback.

## Phase 1 results (v0.8.5)

`PS2_VU1_STATS=1` names each VU1 microprogram start by the hash of VU1's whole code memory and the start address, and prints the
table every 20,000 starts. Three scenes, 60 game frames each:

| Scene | VU1 programs a frame | Code images | Start points | Top 6 starts' share |
| --- | --- | --- | --- | --- |
| Banquet (j_end) | about 2,260 | 11 | 61 | 79% |
| AREA 01 street, flooded (m0105) | about 780 | 14 | 93 | 53% |
| AREA 01 street (m0104) | about 1,150 | 17 | 97 | 57% |

What this says:

- The game swaps VU1's code several times a frame (11 to 17 code images per scene), but the start points repeat across images:
  0x0000, 0x02F0 / 0x0318 (always in pairs: one program in two halves), 0x0618, 0x0A20 / 0x0A48, 0x06A0. One library with
  overlays, not one microprogram per object.
- Two images (7A1A560A and 501D3387) carry most of the world and the characters; a native path for those two covers most of
  what is drawn in 3D.
- Thousands of small batches a frame: the PS2's draw-call pattern. The native path will gather them into few GPU draws.

Next: dump one frame's VIF input for the two main images (the unpacked vertex formats and the matrices the programs read), and
match each start point to what it draws by turning it off in a test build.

### What each program is given (v0.8.6)

`PS2_VU1_STATS=1` now also prints the UNPACKs that arrive before each program most often:

| Start | Given | What it likely is |
| --- | --- | --- |
| 0x0000 | 4 x V4-32 | one 4 x 4 matrix (the object's place on screen) |
| 0x02F0 | 3 x V4-32, or nothing | a header: the GIF tag and the draw settings of the next batch |
| 0x0318, 0x0618, 0x0578, 0x0FD8 | 2 x V4-32, then up to 190 x V4-16 | a batch of vertices, 16-bit fixed point (up to 190 in one batch: the VU1 memory's half for double buffering) |
| 0x0A20, then 0x0A48 | 9 x V4-32, then up to 190 x V4-16 | characters: 9 vectors (bone matrices or lights) before the batch; the banquet (many people) runs these the most |

So the models reach VU1 as compact 16-bit vertex arrays with a matrix per object: the native path can read the same arrays and
matrices from the DMA lists and draw each object as one GPU draw, instead of 100-odd batches.

