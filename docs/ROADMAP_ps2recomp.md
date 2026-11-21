# Roadmap

> **2026-11-12:** evaluating a move to ps2-native-recomp (exoteric33), which already has audio, FMV, a GPU renderer, 2x resolution, quick saves and key remapping. See docs/NATIVE_RECOMP.md.

ps2recomp is experimental. Its own README says VU/GS performance is very poor
and many hardware paths are stubbed. Treat the phases below as a long project,
not a weekend.

**Priority order:** first replicate the game exactly (boot -> menus -> gameplay
with correct graphics, input and audio, matching the PS2), then add features.
Everything from Phase 3 on waits until the game plays correctly.
After gameplay renders and runs, the next two pieces Léo asked for are **audio**
(Phase 3b, SPU2) and the **video codec** for the FMVs (Phase 2b).

## Phase 0 — groundwork ✅ (this drop)
Extraction, measurement, ELF analysis, config generation, game module with
data patches, aspect control, ISO mount, voice redirect mechanism.

## Phase 1 — first boot (in progress)
Done so far: SDK functions identified and stubbed (analyzer + Ghidra merge),
`sceSifInitRpc` passes, guest memory layout fixed for the game's 24.4 MB heap,
all 14 IRX modules load and run in ps2xIOP, disc lookups and ROOT/STR archives
work, double-precision soft-float fixed (PMFHL), IOP file I/O for the sound
driver, game init (items, task groups, sound header) completes, overlays load,
and the first in-game screen (memory-card YES/NO dialog) renders with the main
loop running. Controller input comes from libpad2 HLE (keyboard/gamepad). Boot table and
details: WORKFLOW.md. Game debug printf and the tracer are available.

- Run `rd extract/compare/analyze` on the real discs; decide ISO-mount vs folder mode from `elf_us.md`.
- Ghidra export → `rd config` → `rd recomp` → `rd build --debug --debug-ui`.
- Work through boot blockers using ps2recomp's iteration loop (WORKFLOW §7).
- Milestone: Irem logo / title screen.

## Phase 2 — overlays (`MWo3`) (first pass done)
79 code overlays live in ROOT.DAT as `OVERLAY\DISC\*.BIN` (16 MB). Measured:
- linked for a fixed address, no relocations; six load slots from the table
  at 0x48BE00 (0x718A00, 0x738600, 0x745D00, 0x83FE00, 0x860600, 0x100000);
- loader `FUN_002c99f0(name, slot)` reads the file to the slot, then
  `FUN_00100290` clears the BSS (header +0x14) and runs static constructors
  (`__initialize_cpp_rts`, tables at +0x18/+0x1C);
- menu overlays map name -> slot through the table at 0x43A6B0 (e.g. TITLE
  slot 3, MCMENU slot 2); the ELF's filesz-0 program headers record every
  overlay's address and size (e.g. 0x745D00/0xFA100 = M1_01_00.BIN).
Done (first pass): `rd overlays` extracts them and recompiles each at its
load address under its own names. Function discovery per overlay: stack-frame
prologues, internal `jal` targets, code pointers in the overlay's data, and
main-ELF calls/pointers into the slot that land on a plausible function start
(72 code overlays -> 5,110 functions, 163,683 dispatch entries; 7 ATLAS_n are
data-only). The game module registers an overlay's entry points when the
loader brings it in and drops the previous occupant of that memory.
Open: verify discovery against real execution (missing-target logs inside
overlay space will point at anything missed).

## Phase 2b — FMV
FMVs (PSS: MPEG-2 video + audio, streamed from ROOT.DAT) are skipped for now
(`[movies] skip`). Planned: real playback. Replace the player `FUN_00212bc0` with a
host decoder that reads the same ROOT.DAT range, demuxes the PSS and presents
frames through the runtime's display, honouring the game's skip button. Decoder
options: pl_mpeg (single header, MPEG-1/2, no extra DLLs - fits the static build)
or FFmpeg/libavcodec statically linked. Better-quality option on top: an optional
movie pack (the same movies re-encoded once from the disc to H.264/AV1 at a higher
bitrate, or AI-upscaled) loaded instead of the PSS when present, the same way the
JP voice pack works.

## Phase 3 — QoL
- **PC save system:** hide the PS2 memory-card layer. The memory-card dialog at
  boot (MCMENU overlay, "memory card not found / continue without saving?") and
  every card check/format/select screen should not appear. Saves go to standard
  files (`saves/` next to the exe, or `%APPDATA%`), with numbered slots and
  autosave backups. Hook point: libmc2 (`sceMc2*`, 0x2E9xxx-0x2EFxxx) and the
  game's `T2MCard*` layer (save data 106,864 bytes, system body 142,400 bytes):
  answer them from host files so the game's own save/load menus work, then skip
  the card-only screens.
- Widescreen (patch already wired). Check HUD/menus and add HUD-correction patches.
- `no_post_filter` as an option; the post filter is also the most expensive
  effect on a CPU rasterizer.
- 30 FPS: the EU build targets 30 FPS, and its *NTSC Mode* patch shows the
  game picks a video config at boot (`0x0016E74C` in the EU ELF). The US
  equivalent has to be found by diffing against an EU ELF (not in your folder).
  TCRF notes EU-specific speed bugs, such as cling movement at 2/3 speed, that
  would need fixing too.
- Skip intro logos, fast text, extra save slots: find in Ghidra, patch in the game module.
- JP Map Select (`0x00188110` in JP) → find the US equivalent and expose it as a debug option.

## Phase 3c — in-game "Recomp Settings" menu
Every option in `rawdanger.ini` (display, widescreen, FPS, post filter, voice language,
movies, saves, debug) exposed in a **Recomp Settings** entry in the game's own options
menu (and on the title screen), drawn in the game's menu style, applied live where
possible and written back to the ini. Implementation: hook the options-menu overlay's
item table to add the entry, and render the sub-menu from the game module with the
game's own font/menu routines.

## Phase 5b — modding: texture and audio packs
- **Texture dump:** write every texture the game uploads (GS upload hook: CLUT +
  indexed data decoded to RGBA PNG), named by a content hash so names stay stable.
- **Texture pack:** load a PNG with the same hash from `textures/` instead of the
  original (any resolution; the renderer scales UVs). Needs the Phase 4 renderer.
- **Audio dump:** export the sound banks (SPU2 VAG/ADPCM) and streamed voices/music
  to WAV, named by bank + index.
- **Audio replacer:** WAV/OGG/FLAC with the same name in `audio/` played instead of
  the original - the same mechanism the JP voice pack uses.

## Phase 3e — keyboard and mouse
Full keyboard + mouse support (mouse camera/aim where the game has free look,
rebindable keys, menus clickable) with **key-prompt textures**: every on-screen
button glyph (the game's DualShock 2 icons in HUD, tutorials and menus) swapped
for keyboard/mouse key images matching the current bindings, switching back to
pad glyphs when a gamepad is used. Uses the texture-replacement path (Phase 5b).

## Phase 3f — JP region texture toggle
The JP release (Zettai Zetsumei Toshi 2) has region-exclusive textures (signs,
posters, store fronts, in-world text in Japanese where the US build localised
them). A **Recomp Settings** toggle loads the JP versions of those textures
from the JP disc (built into a pack once, like the JP voice pack), so the city
can look as it did in Japan while keeping the English UI.

## Phase 3b — audio
ps2recomp has no SPU2 emulation yet (registers are stored, nothing is mixed),
so the game is currently silent and sample-bank uploads are answered on the EE
side (`[hle] spu_bank_upload`). Real audio needs:
- an SPU2 model behind ps2xIOP's SPU2 register/DMA range (voices, ADSR, ADMA,
  reverb), or an HLE of Irem's driver at the RPC 0x12346 level;
- resolving why the upload conflicts with IREMSND2's looping PCM streams on
  both cores under ps2xIOP (LIBSD's VoiceTrans refuses a core with ADMAS set,
  which PCSX2 reports the same way, so on hardware something keeps core 1's
  stream off at that point; see WORKFLOW.md boot table, PC 0x2C9504).
This is also where the JP voice swap becomes audible.

## Phase 3d — accurate GS rendering (current priority)
Main menu is reached but drawn distorted. Fix the runtime's GS emulation until
frames match the PS2 (PCSX2 as reference): texture formats and swizzling
(PSMT4/PSMT8/PSMCT16 and the H/HL/HH variants), CLUT loading (CSM1/CSM2, CSA,
TEXCLUT), TEXA alpha expansion, texture wrap/clamp modes, alpha blending/test,
Z test, FST/STQ texture coordinates, sprite/strip primitives, framebuffer
feedback and local-to-local copies, and which buffer is displayed (PMODE /
DISPFB / DISPLAY, interlace). Method: GS captures (F9, patch 0010) replayed
offline with `tools/gs_replay`, fixed in new ps2recomp patches, verified
against the same capture.

## Phase 4 — modern graphics
**Speed:** the CPU rasterizer is the frame-rate limit (title screen ~2 fps before patch 0013).
0013 spreads big primitives over all cores; a GPU backend for `GSRasterBackend` is
what makes the game run at full speed, and it comes before upscaling and the rest.
The runtime's GS is a CPU rasterizer behind an abstract `GSRasterBackend`
(`ps2xRuntime/include/runtime/gs/gs_backend.h`). "Modern graphical settings"
means writing a **GPU backend** for that interface:
- internal-resolution upscaling (2×/3×/4×), proper texture filtering,
- MSAA on geometry, de-dithering, an optional post-filter replacement shader,
- vsync / frame pacing / uncapped presentation.
- correct aspect: today the 640x448 image is stretched to the window; present
  it at 4:3 (or 16:9 with the widescreen patch) with pillar/letterboxing.

This is the biggest piece of work and is probably best upstreamed to ps2recomp.

## Phase 5 — assets
- Hybrid asset set picked per category from `report.md` (e.g. JP audio, JP
  sign/hair textures, US text textures).
- Entry-level voice swap (VOICE_SWAP phase B).
- Installer: a wizard that takes the user's ISO(s) and builds the game folder.
  No game data is ever distributed.
