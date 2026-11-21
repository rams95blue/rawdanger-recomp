# English / Japanese voice switch

`[audio] voice_language = en | jp` in `rawdanger.ini`, switchable without
rebuilding and without modifying either disc.

## What the discs contain

All voice lines live in `/STR/STR.DAT`, indexed by name in `/STR/STR.FAT`
(`VOICE\TITLE.VOI`, `VOICE\MES_01_01_00_013_EVE.VOI`, ...).

- 4,687 lines exist on both discs
- 137 lines are **JP-only**: the `*_EVE.VOI` lines where NPCs call out the
  protagonists' names, plus a few others such as `H_TASUKETE`
- 7 lines are **US-only**, e.g. `HY_AYM35`, `MES_01_01_01_268_LOOK`, `MES_EPILOGUE_PL01_195`

## Build it — `rd voicepack`

`tools/rd_strpack.py` writes `work/us/voice_jp/STR/STR.FAT` + `STR.DAT` (~1.1 GB):

1. Keeps the **US index order**, so lookups by name *or* by index land on the same slot.
2. Replaces every shared line with the JP recording.
3. Keeps the 7 English-only lines (`--mute-us-only` silences them instead).
4. Appends the 137 JP-only lines (the US script never asks for them yet).

Then `voice_map.txt` routes `cdrom0:\STR\STR.FAT` / `STR.DAT` to the hybrid
files. The game opens these by name (no hard-coded sector table, per
`elf_us.md`), so the size difference doesn't matter.

It also prints the first bytes of a few US/JP lines. If the headers look
alike, the US sound driver (`IREMSND2.IRX`) will play JP lines as-is. If
they differ (JP lines are 33% bigger on average, which could be a different
sample rate), that's the first thing to look at when testing.

## Things to watch when testing

- **Subtitle timing:** JP lines are often longer. If scenes advance on text
  timers rather than voice end, lines can get cut off.
- **NPC name callouts:** the 137 JP-only lines only play if the script asks
  for them. Restoring them means finding the US event code that skipped them
  (Ghidra, later phase).
- **Don't use the demo discs as a source:** their banks are partially
  incompatible with retail, which is what broke the first undub.

## Audit of the Japanese audio (v0.7.5)

Every audio file of the two discs compared (tools: rd_hybrid.py prints the counts when it builds the disc):

| What | US disc | JP disc | In the Enhanced Edition |
|---|---|---|---|
| Voice lines (STR.DAT, VOICE\*.VOI) | 4694 | 4824 | 4687 lines with both versions: Japanese (English with the switch off) |
| Lines only on the US disc | 7 (HY_AYM35, four MES_01_01_01_2xx_LOOK, MES_04_03_00_104_EVE, MES_EPILOGUE_PL01_195) | - | English (no Japanese recording exists) |
| Lines only on the JP disc | - | 137 (mostly MES_xx_EVE event lines, H_TASUKETE) | on the disc, never heard; see "The 137 lines only on the JP disc" below |
| Sound banks (SE\*.HD/BD/SQ) | 567 | 567 | 498 the same on both; **69 differ**: the heroes' banks PL01-PL06 (plain, SHOUT, KAKUSEI, RECORDER, WHISTLE) and the scene banks 1-5-1, 4-2-0P0, 4-2-0P3, 4-3-0. These hold voices (shouts, calls, the wake-up and recorder lines) and were re-recorded in English. **Before v0.7.5 the Enhanced Edition kept the English ones**; now ROOT.FAT points to the Japanese banks and the English copies follow, read through the voice switch (recomp.ini [Voices] remap2_off) |
| Movies (MOVIE\*.PSS) | 5 | 16 | the same as the US disc: the in-game movies are small silent screen pictures (128 x 64, no sound track); the JP disc's extra movies are unused versions |

The banks are placed in the sound chip's memory one after another by their size (traced: PL01.BD at 0x56580, the area's
bank right after it), so the bigger Japanese banks fit as they do in the Japanese game. A bank's HD, BD and SQ always come
from the same version.

### The 137 lines only on the JP disc

A voice line is played by name: each map's program (OVERLAY\DISC\Mx_xx_xx.BIN) holds its message records, a record being
the line's name (MES_aa_bb_cc_nnn_EVE, a second name for the other hero) and its text; the program builds VOICE\<name>.VOI
from it. Searching both whole discs for every one of the 137 names:

- **125 of them, and H_TASUKETE, appear nowhere but in the voice index (STR.FAT) on the JP disc as well**: no message of
  the Japanese game names them either. They are recordings left over from development (lines cut before the Japanese
  release, e.g. MES_01_01_00_013-015 next to 011 and 016, which both games use). Neither game ever asks for them; there is
  no scene to play them in.
- **12 are used by the Japanese game** and cut from the US one: 11 in area 4's MES_04_02_00 scene (059-062, 068, 070 ...:
  a stretch of dialogue the US release rewrote; the US program for that map has other messages there) and
  MES_01_01_01_583_EVE in the hotel. Bringing them back would need the Japanese map program for those scenes, whose
  text is Japanese (Shift-JIS, which the US font cannot show) and whose events differ from the US script.

So in the Enhanced Edition every Japanese line that the US script has a place for is used (4687 lines), the sound banks'
shouts and calls included.
