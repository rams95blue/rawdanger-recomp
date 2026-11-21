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
