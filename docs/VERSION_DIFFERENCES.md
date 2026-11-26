# US vs JP — what's known before measuring

Measured numbers come from `rd compare` (`work/reports/report.md`). This page
collects what's already documented, so the report can be read in context.

| | US — Raw Danger! | JP — Zettai Zetsumei Toshi 2 |
|---|---|---|
| Serial / ELF | SLUS-21501 / `SLUS_215.01` | SLPS-25606 / `SLPS_256.06` |
| PCSX2 CRC | 2905C5C6 | C988ECBB |
| Your ISO size | 1,620,541,440 B | 2,311,651,328 B |
| Release | June 2007 (Agetec) | March 2006 (Irem) |
| Target frame rate | 20 FPS | 20 FPS (EU targets 30) |

## Measured on your discs (rd compare + FAT index analysis)

Both discs have the same layout: `ROOT.DAT`/`ROOT.FAT` (game data), `STR/STR.DAT`/`STR.FAT`
(voices only), `IOP/` (identical). The FAT files are name indexes:
`"FAT "`, count, then `{offset, size, name_offset}` records and a name table.

| | US | JP | verdict |
|---|---|---|---|
| Voices (`STR.DAT`, all `VOICE\*.VOI`) | 4,694 lines, 782 MB | 4,824 lines, 1,062 MB | JP has 137 extra lines; shared lines are 33% bigger in JP (format check in `rd voicepack` output) |
| SFX banks (`SE\*.HD/.BD/.SQ`, 189 each) | 66.1 MB | 66.4 MB | effectively identical |
| Textures (`*.TM2`, 19.7k TIM2 pictures) | same resolutions & formats | same | identical quality; JP differs in content (original hair, intact signs) |
| Opening FMV `T2_OP_SHINO.PSS` | **67.5 MB** | 46.2 MB | **US encode is higher bitrate** |
| Extra JP movies | – | ~410 MB of leftover test encodes (`T2_1_1_5_30/300/350/400/500/6000I…`, `_OLD`, `_LOW`) | not real content |
| Models | 49 US-only parts (blonde hair `HAIR_B`, PL01 hands/shoes/pants) | 34 JP-only (Japanese font, test movies) | localization changes |

So the JP disc's extra ~690 MB is ~280 MB of voice data plus ~410 MB of unused
movie test encodes. It doesn't hold better textures or SFX. Best mix: **US base**
(better opening FMV, English text) **+ JP voices**, plus JP hair/sign textures
as a later option.

## Content differences (TCRF)

- **Voices:** in JP, NPCs say the protagonists' names out loud. The
  international versions dropped those lines.
- **Hair:** black-haired characters were recoloured blonde for the
  international release (models unchanged, and not consistently: flashback
  photos and Mayor posters still show black hair).
- **Textures:** some road-sign and similar textures are **corrupted in the
  international versions** and intact in JP.
- **Names:** Tomisaka City → Del Ray City, Haneshiro River → Hudson River.
  Columbia Sportswear tie-in items were renamed.
- **Areas:** an area near Downtown Station (Isaac's chapter) is blocked in the
  international versions but open in JP. A cut area (taxi repair shop, Mayor
  Goldstein cutscene) isn't blocked in JP, where the water plane is much lower.
- **Debug:** a Map Select function exists at `0x00188110` in the **JP** ELF.

## What the 2025 US undub (C&P-kyun) tells us about the data

- **Sound banks** are separate files: the undub uses the retail JP banks, with
  one bank edited to mute a single English-only clip.
- **Streamed voices** live in an **audio archive**, which was *expanded* with
  JP-only entries, and six English-only clips were muted. So the JP archive has
  more entries than the US one.
- **Textures** were brought over selectively, matching renamed localized
  textures to their JP originals. Copying whole JP texture archives (as the old
  undub did) pulls in Japanese text baked into the textures.
- **`MWo3` "module-binary hybrid" files** hold code + data per module; the
  undub author couldn't fit restored names into them.
- The old (emuler) undub copied sound banks from the **demo** discs, which are
  "halfway-incompatible" with retail. **Use the retail JP disc, not your demos,
  as the voice source.**

## Expected reading of the report

- If JP audio has the higher sample rate or more bytes per clip → use JP audio
  wherever the US executable accepts it.
- For textures, prefer JP for the corrupted signs and the original hair. Keep US
  for anything with English text.
- FMV: compare declared bitrates; JP cutscenes may also have burned-in Japanese text.

Sources: TCRF "Raw Danger" page; PCSX2 wiki (Raw Danger!); PCSX2/pcsx2_patches;
retrogametalk.com description of the C&P-kyun USA+UNDUB patch.

## Every other file that differs (v0.8.6 sweep)

All 6,000-odd files of ROOT.DAT compared, voices and textures left out (both already switch in the Enhanced Edition):

| Files | Differ | How | Can it be a toggle? |
|---|---|---|---|
| Character models (`IMD\CHR\...`: heads, hair, hands of the six heroes and 25 other characters) | 86 of 1,273 | Most differ only in a few bytes: the number of the texture they draw with (the US release points to its renamed copies, for example 22001 for Japan's 10004). PL02's hair is a different shape (11,842 bytes differ). | Yes, next to "Japanese faces and hair". First the Japan-only textures they point to (10004, 10017, ...) must go on the disc too, or the swapped models would point to textures the US disc does not have. |
| Columbia items (`ITEM_0137/0138` backpacks, `ITEM_0139` whistle, `ITEM_0404` compass) | 4 + 10 hero copies | One texture number each: the Japanese release shows the Columbia logo | Yes, with the models above (same reason). |
| Maps (`AREA01`-`AREA06`: `M*_*.DAT`, `ATL_*.DAT`) | 231 of 3,248 | 100 differ only in their pictures (signs, already swapped by the "Japanese signs" overrides); 131 differ in size: map data and the scripts with their text | No: the scripts carry the text, in Japanese. This is where the open area near Downtown Station and the lower water in Japan are; they could only come over piece by piece. |
| Menus (`IMENU`, `OPTION`, `TITLE`, `NAVI`, `MEMORIES`, `GAMERESULT`, ...) | 30 | The pictures of the words, in Japanese | Only as "Japanese text" (pending item 4), together with the font (`FONT\FONTALL`, Japan only) |
| Sound banks (`SE\*.HD/.BD/.SQ`) | 69 | Voices inside effects banks | Done: they switch with the voices since v0.7.5 |
| Overlays (`OVERLAY\DISC\*.BIN`) | 79 of 79 | Program code | No: code of each release's own program |
| Movies | 2 | The opening film and 1-1-5 | Done: "Japanese opening film" (v0.8.1) |

## The European release (SLES-54587, v0.10.0, corrected in v0.10.1)

Compared file by file with the US disc (the European one: "Raw Danger (Europe)", SYSTEM.CNF VER 1.02, VMODE PAL):

- **The program** (SLES_545.87): the same 6207 functions in the same order; compiled for 30 steps a second instead of 20 (about
  1150 constants, docs/FRAMERATE.md), PAL video (runs at 25 on a PAL TV). English only. Its texts differ in three documents
  (the documents' table, one entry every 0x20 bytes, text pointers from 0x3CC754 in the US program, 0x3CC6D4 in the EU one):
  - **document 1, the Capitol Newspaper of December 24, 2010** ("New Record Hits Del Ray: Record rain for December" / "Signs of
    a Flu Outbreak?"). The US entry is there, in the same place, but its text pointer points to *Geo's guidelines* (the
    text of document 3 too), so in the US game that newspaper shows Geo's guidelines a second time; its own text is not in the
    US program at all;
  - the investigation report: "collect several key items" (US: "collected"), and "[[Investigation Report]";
  - the report card: "[[Review]". `[[` is how the texts write a plain `[` (one `[` starts a code such as `[PIL,2]`).
- **Dialogue** (in the code overlays, `OVERLAY\DISC\*.BIN`, built for 30 too): typos corrected: "chief" (US "chef"),
  "your mother's memento" ("you"), "gone through" ("though"), "I haven't seen him" ("seen for"), "What are you going to do with
  it?" ("What are going"); the game result screen's "Would you like to save the data?" (US "Do you wish to delete saved
  data?"); two lines made to fit their box ("Her name\nis Kelly Austin", "out of this place" without "dangerous").
- **Pictures**: the character select's stamp on a hero you got through the game reads **"Survived"** in place of 生還
  (CHARSEL.DAT, picture 40; 8-bit in the EU file, 4-bit in the US one); the "TP" label reads "BT" (BOOT.DAT picture 27, the
  item menus IMENU_PL01-06 and IMR_1-6); the title's line "Published by 505 Games"; the ending's credits list 505 Games'
  localization team and staff in place of Agetec's (M1_09_00, M2_08_00_I01, M6_01_00). (v0.10.0 put the stamp in BOOT.DAT by
  mistake: BOOT.DAT's one difference is the TP/BT label.)
- **The tutorial**: one hint differs. Where the US tutorial says "Tilt the left analog stick to Walk", the European one says
  "If you lose your footing while walking, you can still hang from a structure", and its voice line
  (VOICE\MES_01_99_00_073_EVE.VOI, 4.2 s in place of 2.3 s) is that other hint: a line from another part of the tutorial put
  in the wrong place. **Not used.**
- **The same**: the IOP modules, 4693 of the 4694 streamed files (voices, music, movies), 5694 of the 5791 data files.

### What the Enhanced Edition takes from it (v0.10.1)

The Enhanced Edition needs the European disc (five discs: US, EU, JP and the two JP demos). When it makes its disc,
`tools/rd_eu.py` (called by `rd_hybrid.py --eu`) reads your EU image and takes the following (`rd.py eu US.iso EU.iso`
lists them without making a disc):

| What | How |
|---|---|
| The "Survived" stamp | CHARSEL.DAT picture 40 drawn in the US picture's own format (16 colours: the EU stamp is 129 shades of one red, brought down to 16; the US file keeps its size) |
| The newspaper (document 1) | Its text from the EU program, in the 8 KB below the program (0xFE000: user memory the game never touches, zero in all 18 save states checked), the US entry's pointer set to it |
| The two documents' corrections | In place (they fit where the US texts are) |
| The dialogue typos and the two lines that did not fit | Written over the US words in the overlays (each fits in the room the US line has; "What're you going to do with it?" for the M4_02_00 line, the EU words being 2 letters too long) |

The program's texts go in as a patch file the disc maker writes next to the disc (`hybrid.iso.eu.pnach`, section
`[EU text fixes]`; written into the program before it is translated and again every frame, so a save state made earlier
gets them too). Nothing from the EU disc is in this project. Not taken: the TP/BT label, the 505 Games credits and title line,
the tutorial hint and its voice line, the 30-steps program.
