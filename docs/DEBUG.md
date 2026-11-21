# The developers' debug features left on the disc

Raw Danger's program still carries the menus its developers used. Two of them are scenes of the game's own scene list
(the table at 0x3A8380 of SLUS_215.01, 24 bytes a scene: name, setup function, update function):

| Scene | Name     | What it is                                                                                  | In this build |
|-------|----------|---------------------------------------------------------------------------------------------|---------------|
| 40    | DEBSEL   | the debug menu: MAP SELECT, PLAYER MOVE, LEVEL, ITEM, FLAG EDIT, FOG EDIT, SYS INFO, GAME INFO, DEATH, DAMAGE, MAP DISP, BOX DISP, PAUSE, SKIP ICON, TEX LIST, SE TEST, ADD ITEM, EXIT GAME | not offered (see below) |
| 41    | MAP SEL  | the map select: any area, scene and gate of the game, loaded at once                        | **F7** in the game |

## Map select (F7)

- **Open it**: press **F7** while playing (or F1 -> Settings -> Game -> *Developers' map select*). It opens only from the game
  itself (scene 21): over the title or the menus the map select shows but the title keeps the pad.
- **Use it**: up / down picks a line, left / right changes its value, **Cross** (Space / Enter) loads the map.
  - **AREA**: the part of the story (00 = "NO SCENE", nothing to load).
  - **MAP**: shown as `MAPaa-bb-cc` (area, block, sub-map, the names of the map files `AREAaa\Maa_bb_cc_*.DAT`) once an
    area is picked; left / right steps through them.
  - **GATE**: the entrance of the map you arrive at (0 is the first).
- **Leave it**: load a map (it is the only way out the game gives; switching the scene back by hand leaves the map select
  running over the game).
- **What it does not do**: it does not set the story's flags. A map loaded ahead of the story can have closed doors,
  missing people or events that wait for things not done yet. Quick save (F5) first; quick load (F8) brings you back.

How it is opened: the game switches scenes through a "next scene" word (0x48F48C, read each frame by the scene switch at
0x16ED80; 0x48F488 is the scene it runs, 0x48F484 the previous one). The engine writes 41 there and keeps writing it for
up to 3 seconds until the game has switched (the game's own pause holds the switch). The launcher sets
`PS2_DEBUG_SCENES=0x48F48C;0;41;21` (the word, the debug menu's scene (0: not offered), the map select's scene, the scene it
opens from).

## The debug menu (DEBSEL, scene 40): its text, and why it is still not offered

It opens (`PS2_DEBUG_SCENES=0x48F48C;40;41;21` turns it on, Shift+F7). Its strings are **Shift-JIS full-width letters** from
the Japanese developers' build (ＭＡＰ　ＳＥＬＥＣＴ, ＳＯＵＮＤ　ＴＥＳＴ, ＥＸＩＴ, and a kanji title), and the US program prints
them byte by byte through its ASCII font sheet: each lead byte (0x81 / 0x82) lands on an empty cell of the sheet (the grey
checker boxes) and each trail byte on an unrelated letter (the "one letter lower" text, `l`o@rdkdbs`, is the trail bytes of
ＭＡＰ　ＳＥＬＥＣＴ: 0x826C is Ｍ).

v0.7.8 (engine patch 0024, `PS2_SJIS_TEXT=3592`, the US font sheet's texture base, set by the launcher): a sprite of that
sheet on a lead byte's cell is not drawn, and the trail's sprite is moved to the same character's ASCII cell (full-width
letters, digits, space, signs and arrows), packed along its line; kana and kanji (the title, apart from its "2") have no
letter in the US font and are not drawn. The menu now reads MAP SELECT / SOUND TEST / EXIT. It is still not offered: in the
tests it did not answer the pad (Cross on MAP SELECT did nothing), so it could only be left by closing the game.

The **map select** (F7) used the same font with its 24-pixel letters stretched to 32 with filtering (soft, the lines touching):
while a developers' scene is on screen they are drawn at the font's own size (crisp, a gap between the lines).

## Other leftovers

- The program prints "MAP Wether" and other debug strings that nothing shows; the engine reads the weather word it sets
  (0x48F310) for the rain on the lens.
- SE TEST, TEX LIST and FLAG EDIT exist only inside DEBSEL.
