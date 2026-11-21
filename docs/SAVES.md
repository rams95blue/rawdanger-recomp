# Saves without the memory card

Asked for after v0.7.1: by default no memory-card screen when the game starts, and a save system that does not depend on the
PS2 memory card.

## v0.7.8: the saves are plain folders

The launcher keeps the saves in **`saves`** beside RawDangerPC.exe, one folder per save, shared by every build:
`saves\BASLUS-21501` (the system data: options, unlocks), `saves\BASLUS-21501000`, `...001` ... (the save slots), and
`saves\BISLPS-25606...` for the Japanese build. Each folder holds the save's files as the game wrote them, and `_card.json`
(the card's directory entries: dates, modes, attributes, so the card is made again exactly).

- Before each start the build's card image (`<game>.ps2` in the game's folder) is made from the folders: a freshly formatted
  card with every save of this build; a slot the other language's build saved later is brought over under this build's name.
  The language switch (F1 -> Language) needs no copying of its own any more.
- After the game ends, the card's saves are written back: only the ones that changed, each written whole into a new folder
  that then takes the old one's place; a save the game deleted goes to `saves\_deleted`. A run whose end the launcher did not
  see (a crash) is written back at the next start.
- The first time, the build's old card is read in (nothing on it is taken as deleted) and kept as `<game>.ps2.before-save-folders`.
- The card image is now only the game's working copy: no formatting, no free-space limits to manage, and a save can be
  copied, backed up or shared as a folder. `tools\rd_card.py export / build` do it (tested: card -> folders -> card ->
  folders gives the same bytes, and mcformat.py's check passes on every card made).
- Not done: the game's own screens still say "memory card" (plan step 3 below).

## Notes from the investigation

This part keeps what was found (US program, SLUS-21501; the JP program has the same code at other
addresses, see `tools\rd_card.py` for the save format both share).

## The boot order (traced: the scene word 0x48F488, every frame)

| Frame (test run) | Scene | Name |
|---|---|---|
| 35 | 1 | BOOTLOAD |
| 45 | 3 | MCCHECK: "Checking memory card", then either "Loading system data..." or "Starting the game without using System data" (waits for a button) |
| 313 | 4 | HDDCHECK |
| 317 | 5 | HTCHECK |
| 321 | 2 | WARNING |
| 325 | 9 | OPMOVIE (LOGO, 6, is skipped by `[Skip intro logos]`) |
| 1013 | 11 | TITLE |

Scene records: a table at 0x3A7FE8, 24 bytes each (flags, ?, name, set-up task, update, 0x80000000). MCCHECK: name 0x457138,
0x182250, 0x2A2160. 0x2A2160 sets the card flow's mode (field 0x80 = 1), starts the tasks 0x2A1F20, 0x2A1EE0 and 0x2A2020; the
states are functions in a table at +0x140; the flow ends by going to scene 4 (0x2A1FA8). The scene is set with 0x16ED40(id).

## The card layer

- T2MCard (the game's own layer, 0x29C000-0x29F000): one global block at 0x695260; slot -1 is the system data
  (`/BASLUS-21501/BASLUS-21501`, T2MCARD_SYSBODY 0x22C40 bytes), slots 0.. the game saves (`/BASLUS-21501%03d/...`, SAVEDATA
  0x1A170 bytes). Paths are made by 0x29DEF0 / 0x29DF60 / 0x29E050 / 0x29E0F0 into 0x695260+0x64 / +0x164.
- sceMc2 (Sony's newer memory-card library, linked into the program, 0x2E6000-0x2F0600): the file system runs on the EE. The
  synchronous calls (0x2E9C00-0x2E9F40) start an asynchronous one (0x2E8AD0-0x2E9808) and wait; a worker thread (0x2E98F0)
  runs one function per command (0x2EAFB0, 0x2ED4A0, 0x2ED6A8, 0x2EB348, 0x2ECF10, 0x2EDC88, 0x2EDEB8, 0x2EDFE0, 0x2EE500,
  0x2EE688, 0x2EBC78, 0x2ECE20, 0x2EE808). Replacing those functions with ones that read and write plain files (one file per
  save, in a folder next to the game) gives saves that need no card image, no formatting and no free-space checks, while the
  game's own DATA SAVE / DATA LOAD screens (20 cases) stay.
- The messages (MCMENU.BIN overlay, loaded at 0x738600, file offset - 0x40 + 0x738600): a table of 32-byte records chosen by
  0x73A950 from (command, result, ...); "Starting the game without using System data" is 0x73E7F0, its record near 0x73C714.

## Plan

1. The card flow at boot answers itself: system data is loaded when there is some, without the notice; with none the game
   goes on at once (no button).
2. Saves in files: the sceMc2 worker's functions replaced (engine hook, host code) so `/BASLUS-21501.../...` paths are files
   under `games\<name>\saves\`; the old `.ps2` card is read once to bring its saves over.
3. The in-game "Do not remove memory card" texts changed to plain "Saving..." / "Loading...".
