# The two JP demo discs compared with the retail game

*Zettai Zetsumei Toshi 2 Taikenban* Type-A (SLPM_602.54) and Type-B (SLPM_602.73), compared with the retail JP program
(SLPS_256.06; the US program SLUS_215.01 has the same 42 scenes and 45 maps). Made from the three programs with
`tools\rd_demos.py`; run `rd.py demos` to make it again from your discs (it also reads each disc's ROOT.FAT, which this
page could not: the demo .bin files are larger than what could be copied for this comparison).

## In short

- **Type-A** is an early build. Its scene table uses 20-byte entries (retail: 24), it has 27 of the 42 scenes and knows
  only the 4 maps of the opening building (M1_00_00, M1_01_00, M1_01_01, M1_99_00). Missing: the ending movie, loading
  from Game Over, the memory-card menu, cooking, the item-select menu, the timetable, navigation, the chapter result and
  all post-game extras. Its program still contains things the retail game moved out or dropped: the opening building's
  event scripts by name (`EVENT_01_01_01_...`, retail keeps them in the area code overlays), map atlas file names for
  areas 1 and 2, a tutorial paper item (`ITEM_0086_PAPER_TUTORIAL`) and developer prints for textures
  (`%8d byte, %s, TEX ID=%s`, `TexGetTEXimage( %d ) is Error`).
- **Type-B** is close to retail: 35 scenes, everything up to the chapter result. It lacks the post-game and extra
  scenes (Showcase, Radio, Omoide, Memories, Compass, episode load/save). It knows 28 maps across areas 1-5 and **names
  three maps the retail game does not have: M3_10_01, M3_12_00 and M4_01_00**; the retail discs carry no data under
  those names. It also lists NPC clothing parts by path (`IMD\CHR\NPC01..06\PARTS\ITEM_05xx_COAT...`), a save-slot debug
  line (`%02d : CHAR(%d) MAP(%d-%d-%d G%d) TIME(...) SAVE(%d)`) and its own memory-card folder `BISLPM-60273`.
- Both demos keep the developer scenes DEBSEL (40) and MAP SEL (41), like retail.

## What it means for the Enhanced Edition

The three Type-B-only maps are the interesting part. If the Type-B disc carries their data (`rd.py demos` says, under
"Type-B maps"), they are cut or changed areas that could be looked at, and possibly loaded through MAP SEL once that works.
Nothing from Type-A is missing in retail apart from developer text.

## Scenes

| scene | Type-A | Type-B | retail |
|---|:-:|:-:|:-:|
| 0 DUMMY | ✓ | ✓ | ✓ |
| 1 BOOTLOAD | ✓ | ✓ | ✓ |
| 2 WARNING | ✓ | ✓ | ✓ |
| 3 MCCHECK | ✓ | ✓ | ✓ |
| 4 HDDCHECK | ✓ | ✓ | ✓ |
| 5 HTCHECK | ✓ | ✓ | ✓ |
| 6 LOGO | ✓ | ✓ | ✓ |
| 7 PUBLOGO1 | ✓ | ✓ | ✓ |
| 8 PUBLOGO2 | ✓ | ✓ | ✓ |
| 9 OPMOVIE | ✓ | ✓ | ✓ |
| 10 ENDMOVIE |  | ✓ | ✓ |
| 11 TITLE | ✓ | ✓ | ✓ |
| 12 NEW GAME | ✓ | ✓ | ✓ |
| 13 LDGAMEOV |  | ✓ | ✓ |
| 14 LOADGAME | ✓ | ✓ | ✓ |
| 15 NAMEEDIT | ✓ | ✓ | ✓ |
| 16 SETTING | ✓ | ✓ | ✓ |
| 17 TUTOMENU | ✓ | ✓ | ✓ |
| 18 MCMENU |  | ✓ | ✓ |
| 19 GAMELOAD | ✓ | ✓ | ✓ |
| 20 GAMERES | ✓ | ✓ | ✓ |
| 21 GAME | ✓ | ✓ | ✓ |
| 22 GAMEOVER | ✓ | ✓ | ✓ |
| 23 IMENU | ✓ | ✓ | ✓ |
| 24 IRESENT | ✓ | ✓ | ✓ |
| 25 ICOOKING |  | ✓ | ✓ |
| 26 ISELMENU |  | ✓ | ✓ |
| 27 MAP | ✓ | ✓ | ✓ |
| 28 IM_READ | ✓ | ✓ | ✓ |
| 29 RELATE | ✓ | ✓ | ✓ |
| 30 TIMETBL |  | ✓ | ✓ |
| 31 NAVI |  | ✓ | ✓ |
| 32 RESULT |  | ✓ | ✓ |
| 33 SHOWCASE |  |  | ✓ |
| 34 RADIO |  |  | ✓ |
| 35 OMOIDE |  |  | ✓ |
| 36 MEMORIES |  |  | ✓ |
| 37 COMPASS |  |  | ✓ |
| 38 EPI LOAD |  |  | ✓ |
| 39 EPI SAVE |  |  | ✓ |
| 40 DEBSEL | ✓ | ✓ | ✓ |
| 41 MAP SEL | ✓ | ✓ | ✓ |

## Maps (bold: not in retail)

| map | Type-A | Type-B | retail |
|---|:-:|:-:|:-:|
| M1_00_00 | ✓ | ✓ | ✓ |
| M1_01_00 | ✓ | ✓ | ✓ |
| M1_01_01 | ✓ | ✓ | ✓ |
| M1_01_05 |  |  | ✓ |
| M1_02_00 |  | ✓ | ✓ |
| M1_03_00 |  |  | ✓ |
| M1_04_00 |  |  | ✓ |
| M1_05_00 |  |  | ✓ |
| M1_05_01 |  |  | ✓ |
| M1_05_02 |  |  | ✓ |
| M1_06_00 |  |  | ✓ |
| M1_07_00 |  |  | ✓ |
| M1_09_00 |  |  | ✓ |
| M1_99_00 | ✓ | ✓ | ✓ |
| M1_99_99 |  |  | ✓ |
| M2_01_00 |  |  | ✓ |
| M2_02_00 |  | ✓ | ✓ |
| M2_03_00 |  |  | ✓ |
| M2_04_00 |  | ✓ | ✓ |
| M2_05_00 |  |  | ✓ |
| M2_06_00 |  |  | ✓ |
| M2_07_00 |  |  | ✓ |
| M2_08_00 |  |  | ✓ |
| M3_01_00 |  | ✓ | ✓ |
| M3_02_00 |  | ✓ | ✓ |
| M3_02_01 |  | ✓ | ✓ |
| M3_03_00 |  | ✓ | ✓ |
| M3_05_00 |  | ✓ | ✓ |
| M3_06_00 |  | ✓ | ✓ |
| M3_06_01 |  | ✓ | ✓ |
| M3_07_00 |  | ✓ | ✓ |
| M3_08_00 |  |  | ✓ |
| M3_09_00 |  | ✓ | ✓ |
| M3_10_00 |  | ✓ | ✓ |
| **M3_10_01** |  | ✓ |  |
| M3_11_00 |  | ✓ | ✓ |
| **M3_12_00** |  | ✓ |  |
| M3_15_00 |  |  | ✓ |
| **M4_01_00** |  | ✓ |  |
| M4_02_00 |  | ✓ | ✓ |
| M4_03_00 |  |  | ✓ |
| M5_01_00 |  | ✓ | ✓ |
| M5_02_00 |  | ✓ | ✓ |
| M5_03_00 |  | ✓ | ✓ |
| M5_04_00 |  | ✓ | ✓ |
| M5_06_00 |  | ✓ | ✓ |
| M5_07_00 |  | ✓ | ✓ |
| M6_01_00 |  |  | ✓ |

