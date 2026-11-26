# Picture settings and modern graphics: what is in, what is next, what cannot be done

In the game: F1 -> **Graphics and sound** (saved in `keys.ini` next to the memory card).

| Setting | What it does |
|---|---|
| Internal resolution | 640 x 448 (the console's) times 1 to 4, or **auto**: the one whose picture is at least as tall as the screen, so the window only ever shows it reduced (supersampled), never enlarged; no filter is needed then. Chosen when the game starts. Above 1x the engine draws everything with its compute painter (the fast drawing path needs 1x), so it costs more. |
| Game runs behind the F1 menu | **On** (default): the game goes on while the F1 menu is open (its input is held back); off: it waits, as before. With *Pause in background* on, leaving the window still makes it wait. |
| Anti-aliasing | Off, *smooth edges* (an edge-direction blend along jagged stairs, FXAA-like, done while enlarging) or **SMAA 1x** (on the console picture before it is enlarged: finds edges by brightness, measures each edge's length and shape, and blends just enough across it; a port of the MIT reference implementation, HIGH preset). |
| Upscale filter | How a smaller picture is enlarged to the window. **Raw pixels** (the default now): every console pixel stays a hard square, only the border between two pixels is blended over one screen pixel, so a window of any size shows clean, even pixels without a blur. Soft (TV): a plain blend. Sharp: a cubic enlargement with light contrast-adaptive sharpening. **FSR 1**: AMD's edge-adaptive enlargement (EASU): finds the direction and length of the edge under each screen pixel and stretches a sharp lobe along it, clamped to the nearest source pixels, so slanted edges and text grow clean without blur or halos; best for a 1x picture on a big screen. Each also with *smooth edges* (an edge-direction blend along jagged stairs, FXAA-like). |
| Whole-number scaling | The picture is enlarged by a whole number only (1x, 2x, 3x of the console's rows), so every row has the same height; black borders take the rest of the window. |
| Rain on the lens | **On** (default) / off: as hard as the map's rain (light, steady, heavy: the game's weather word, eased in and out), with random gusts and lulls; drops land on the camera while playing (not in menus or films: the launcher passes Raw Danger's scene word, `PS2_LENS_WHEN=0x48F488=21`), cling, run down and fade; each one shows the picture behind it flipped and drawn in, like a water bead. Our own effect, written for this project (see below). |
| Master volume | 0-100 %, 10 % steps. |

## Anti-aliasing at a high internal resolution

SMAA works (a debug view of its passes shows the edges found, the weights and what the blend changes), but at the internal
resolution the game normally runs at (AUTO: the picture at least as tall as the screen, then reduced to the window) the picture is
already supersampled: the stair steps it would smooth are smaller than a screen pixel, so switching it shows little or nothing.
Anti-aliasing matters at 1x and 2x. v0.7.7 also reads SMAA's search table with point filtering, as the reference does.

## Original PS2 Post Processing Filter (the game's colour filter, the old "blur filter")

In short: **on by default since v0.8.0.** It is now GPU friendly (one GPU pass instead of about 2,800 small draws a frame, the
same picture to the last bit), and the blurry picture people knew from it is fixed (v0.7.5: the blur was not the filter's
intent but the way the copy of the frame it works on was read; read crisply, only the intended glow, tint and haze remain).
Switch: F1 -> Settings -> Picture, the game's own Options list, or RawDangerPC.exe (recomp.ini `[Original PS2 Post Processing
Filter]`, on = the filter; up to v0.7.9 the section was `[No post-processing filter]`, on = no filter).

**v0.8.1: the maps differ.** The filter is not one recipe: in the banquet hall the haze comes from two depth palettes and the
last writes from palettes; in AREA 01 (MAP-01-05 and the other flooded streets) from three depth palettes, and the last writes
are a flat colour and a small constant. v0.8.0 knew only the first recipe, so AREA 01 came out grey or with untinted cut-off
parts. Since v0.8.1 the engine draws the first 64 x 32 page of the walk the game's way while it writes the steps down, then
repeats exactly those steps for the rest of the frame in one pass; a walk with a step it does not know is drawn the game's way
to the end. Tested byte for byte against the game's own draws in both recipes (the frame is identical; only the game's scratch
buffers differ). To compare yourself: F1 -> Settings -> Picture -> "drawn: one GPU pass / the game's own draws" (keys.ini
`post_processing = fast / original`; the second is slow, as before v0.7.8).

The details:

The option the PCSX2 wiki calls *problematic post-processing* (recomp.ini *No post-processing filter*; the F1 menu's *game's blur
filter*) skips two calls each frame (0x11E104 `jal 0x19A970`, 0x11E10C `jal 0x22A3B0`). What they draw, from the GS draw trace
of one gameplay frame (PS2_TRACE_DRAWS) with the filter on and off:

- The finished frame (640 x 448 at 0x1180) is walked one 64 x 32 page at a time, 140 pages. For each page: 64 sprites of 8 x 8
  copy it into a scratch page as 16-bit (a channel shuffle), two 8-bit palette (CLUT) passes per channel read it back into a
  third page with additive blending, the fixed weights 0x26, 0x4B, 0x0E (38, 75 and 14 of 128: the 0.299 / 0.587 / 0.114
  brightness weights) making the pixel's brightness; then the page is written back over the frame through four more palette
  lookups with channel write masks (alpha only, then colour only): the colour is mixed toward a tinted curve of its
  brightness by an amount the palettes give.
- Measured on the picture (same frame, filter on against off): a pure colour grade, no blur: each channel mixes 20 to 40 % of
  the brightness back in (less saturated), the blacks rise by about 9 to 12 levels, blue more than red (the misty, cold look).
  Fitting a blurred picture adds nothing (its weight comes out 0).
- The blur everybody saw came from the way the filter reads the frame: on the console (and in the engine before v0.7.5, the
  one-texel tent of patch 0020's RT_TENT) the copy-back was filtered between pixels. With the copy read crisply only the
  intended colour grade is left. The game was not broken from the start: its colour grade is made in a way that also softened
  the picture.
- What it does exactly (worked out from the draws, then checked against the game's own result to the last bit on every pixel
  of a gameplay frame, software renderer): after its bloom (a bright pass at 0x1E, three blur passes in a 256 x 128 buffer,
  added back at 10/128), per pixel: L = brightness from B, G, R with the weights 14, 75, 38 of 128 (each through palette
  34B0); d = a depth index from bytes 2 and 1 of the depth (palettes 3510 x16 and 34F0 /16); the colour mixed toward a tinted
  curve of L by a palette amount of d (35B0, a constant 0x22 here: the desaturation), then a tint of L added by another amount
  of d (35D0 into 3550, more for far pixels: the cold haze); the alpha is that amount. The palettes are built each frame
  from the scene's presets, so every scene has its own grade.
- Why it cost so much: about 2,800 small draws a frame (the frame walked one 64 x 32 page at a time, each draw reading what
  the one before wrote); the engine ran 2,065 GPU batches a frame with it against 102 without.
- **v0.7.8: one pass** (engine patch 0023, `PS2_GAME_FILTER_PASS=1`, set by the launcher): the walk is recognised in the
  GS stream, its draws are left out (their palettes and weights noted as they pass), and the formula runs once over the frame:
  one GPU pass over every sample, at any internal resolution. The same picture to the last bit (software renderer; GPU at 1x
  and 2x). GPU batches a frame 2,065 -> 105; 300 frames at 1x: 78 s -> 37.6 s (35.6 s with the filter off). With it this
  cheap, the filter stays on as the game meant it (v0.8.0: on by default).

## Rain on the lens and FusionRainDroplets

ThirteenAG's [FusionRainDroplets](https://github.com/ThirteenAG/FusionRainDroplets) is GPL-3.0 and hooks Direct3D/OpenGL/Vulkan
games and PCSX2 with per-game data; our engine draws with WebGPU and is MIT, so its code is not copied in. The effect here is
written from scratch in `blit.wgsl` (drops_over) and `host/lens_rain.cpp` (the drops' life). Next: drops only outdoors and
when it rains (the game's weather/indoor flag is not found yet), more drops when the camera looks up, splashes from water.

## Modern techniques

| Technique | Verdict |
|---|---|
| SMAA 1x | **In (v0.6.6).** Three compute passes on the console picture (edges, blending weights with SMAA's two lookup tables, neighbourhood blending), the reference's bilinear reads done by hand on buffers; runs before the enlargement, so it works with every upscale filter. |
| FSR 1 (EASU) | **In (v0.6.6)** as an Upscale filter (F1 -> Graphics and sound, or F2). MIT, written out in the presentation shader. RCAS, FSR's sharpening pass, is left out: it needs the enlarged picture in a texture first, and the picture goes straight to the window; EASU alone already keeps edges crisp. |
| DLAA / DLSS | **Not possible here.** NVIDIA's SDK is closed, runs only through Direct3D 11/12 or Vulkan on RTX cards, and needs motion vectors for every pixel; the PS2's drawing gives none, and our renderer is WebGPU. A temporal AA without motion vectors smears moving things. |
| TAA | Same problem (no motion vectors); not planned. |
| Ambient occlusion (SSAO / GTAO) | **Research.** It needs the scene's depth: the engine keeps the GS depth buffer at 1x, but the game also draws 2D (text, HUD) and its own post filter over the 3D, and the depth format changes between scenes, so AO would darken the HUD unless the 3D part is known. Step 1: read Raw Danger's depth buffer after the 3D and before the HUD (a GS register change marks it). |
| Higher-resolution textures | With upstream's `PS2_TEX_OVERRIDES` (in the engine update list): PNG replacements, after the engine update. |
| Japanese textures | **In (v0.6.7), Enhanced Edition.** Both versions of the 151 textures the US release redrew are on its disc; F1 -> Game options -> *Japanese faces and hair* (79: the characters as the Japanese release draws them) and *Japanese signs and packets* (72: the writing in the world). |

## Ambient occlusion (v0.8.2)

F1 -> Settings -> Picture -> Ambient occlusion: off / light (the default) / strong (keys.ini `ambient_occlusion`). Soft shading
where surfaces meet: feet on the floor, under tables, corners, folds. It is worked out from the game's own depth buffer, on the
3D picture just before the game's colour filter, so the filter tints the shading like the rest of the scene and the HUD is never
touched. The game's depth is about 1 / distance, which is linear across the screen on a flat surface: for pairs of points on
opposite sides of a pixel, a floor or a wall gives exactly the pixel's own depth on average, while a crease gives a nearer one.
Only that difference darkens, so flat surfaces stay clean, and points much nearer (a person far in front of a wall) do not count,
which keeps the dark halos of a plain screen-space AO away. The GPU renderer only, and it needs the PS2 post-processing filter
on (it runs where the filter starts: with the filter off there is no point where the 3D picture is done and the HUD not yet drawn).

v0.10.1 (engine patch 0045): the tutorial had no AO at all. Its filter is not the depth-based one (a glow made from the picture:
the 3D picture is copied into a 24-bit buffer 320 wide, then worked on a page at a time), so the place the AO waited for never
came. Now, when no depth filter has been seen for 60 frames, the AO runs when the finished 3D picture (the full-width buffer
drawn with the depth test) is first read by a textured sprite into another buffer: the 3D is done, the HUD not drawn yet. Once
a frame; a narrower 3D picture (a picture in the picture) is left alone. Checked in the tutorial's opening (the Relief Center):
Settings > Debug > Picture shown: AO alone shows it; the maps with the depth filter are unchanged (it never fires there).

v0.8.7: stronger, as it hardly showed (in the banquet hall, 0.3 % of the picture changed): it looks 80 pixels out at the nearest
depth (was 38, up to 40 pixels), counts creases from 0.4 % (was 0.6 %), with a gain of 3 (was 1.4); light darkens up to 66 %,
strong up to 96 %. Now feet stand on the floor and the tables' edges sit on it.

v0.8.9: measured in the world. The game's depth grows as the distance shrinks, about 131072 / metres (the banquet hall's floor
at the camera reads 68000, the far city 500), so v0.8.7's thresholds, taken relative to the
depth, broke both ways: far away one step of the depth is half a metre, and the far buildings got dark outlines and blotches;
near the camera the 40-pixel radius smeared dark patches under people and over table cloths. Now each point looks about half a
metre round it (depth / 900 pixels, at most 16), counts creases of 2 to 20 cm and only things less than a metre in front of it,
fades out between 20 and 45 m, and darkens at most 72 % (strong) or 50 % (light) of the way. Tuned on depth dumps of four
scenes (the banquet hall, a view over the city, two streets) with a copy of the shader in numpy before it went in.

## The wet look (v0.9.5)

Raw Danger keeps how wet each worn thing is (0-100, what the HUD's drops show) but the hero looks the same soaked or dry. The PC
version darkens the hero as the clothes get wet.

- **Which pixels**: no texture belongs to "the hero" in a way the GPU can see (the textures are uploaded sorted by material,
  mixed with everyone else's), so the hero is found on the screen instead. Each frame the engine takes the hero's bones (the
  bones' world places, see docs/CAMERA.md) and projects twelve limbs (torso, neck, head, arms with the hands, legs with the feet,
  the skirt or the coat tails) with the game's own world-to-screen matrix (0x5FD690) into capsules: two ends, a radius, and the
  nearest and farthest depth.
- **The look**: the AO pass (before the game's colour filter) darkens the pixels inside a capsule whose depth is between the
  capsule's nearest and farthest by up to 38 % and deepens their colours by 25 %, with a soft edge, eased in and out as the
  wetness changes (about two seconds).
- **What it misses**: a shoe or a raised hand can fall just outside the capsules, and the look has no highlight yet.
- Settings > Picture > Wet look (keys.ini `wet_look`, on); Settings > Debug: a forced wetness and the capsules drawn in blue.

## Debug views (v0.9.6)

Settings > Debug > Picture shown draws, in place of the 3D picture (before the HUD and the game's colour filter): the ambient
occlusion alone, to tune or report it; the depth as grey by distance (log scale, 1 m white to 60 m dark, the sky black), to see
what the AO and the wet look see; or the wet look's capsules in blue. Stats on screen puts FPS, CPU / GPU, memory and the scene
at the left of the picture.

