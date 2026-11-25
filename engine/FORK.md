# Our fork of ps2-native-recomp

`engine\ps2-native-recomp` is rawdanger-recomp's fork of [exoteric33/ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp)
(MIT, see its `LICENSE`), shipped inside the project so `RawDangerPC.exe` needs nothing else.

- Upstream base: commit `9b0eef16ecd5df327eb23eb5adaf844251c2f29f` (2026-11-18; before: `0cc2658` of 2026-11-11, updated in v0.6.5 with
  68 upstream changes: film decoder speed-ups, interrupt and sound DMA timing, faster drawing, translator fixes for delay
  slots, VU fixes, CHD discs, texture overrides).
- Our changes on top, each also kept as a patch in `patches\native\` so they can be re-applied on a newer upstream:
  - `0001-pnr-game-printf-and-sifdma-trace.patch`: syscall 0x7D prints the game's own debug text (used with
    `SLUS-21501_2905C5C6.debug.pnach`), `PS2_TRACE_SIFDMA`.
  - `0002-recomp-options-page.patch`: the F1 menu's **Recomp options** page (also F3): the game's patches from
    `games\<name>\recomp.ini` (written by RawDangerPC.exe, passed in `PS2_RECOMP_OPTIONS`); live ones (data-only, like
    widescreen) switch while the game runs and are written back, built-in ones are listed. New file host/recomp_options.cpp.
  - `0003-achievements-and-disc-remap.patch`: offline achievements (`host/achievements.cpp`: RetroAchievements-style
    conditions read from `PS2_ACHIEVEMENTS`, unlocks kept in `PS2_ACHIEVEMENTS_UNLOCKS`, a notice when one unlocks, F1 page
    **Achievements**, also F4); `ps2::disc::set_remap` (a range of disc sectors read from another range, used by the
    Enhanced Edition's live switch between Japanese and English voices: `remap_off = FROM TO COUNT` in recomp.ini);
    `PS2_TRACE_DISC_ALL=1` prints every disc read.
  - `0004-switchable-code-patches-and-achievements.patch`: code patches switchable in the game. pnach.py already turns
    a patch line guarded by a game variable into a "toggle" (two versions of one instruction, chosen at run time); the
    translator now also does this in delay slots and for a `jal` switched with a `nop`. Raw Danger's *Skip intro logos*
    and *No post-processing filter* are written that way, with their switch variables at 0x7F00 / 0x7F04 (free EE kernel
    memory), which recomp.ini writes (an option word no patch file lists becomes a per-frame patch). Achievements: the
    RetroAchievements condition syntax as rcheevos reads it (see host/achievements.hpp), unofficial ones on request
    (`PS2_ACHIEVEMENTS_UNOFFICIAL=1`), each one switchable (`<unlocks>.off`; Enter on the in-game Achievements page).
  - `0005-raw-danger-menu-perf-log-widescreen.patch`: the quick menu laid out for Raw Danger (Resume, Quick save/load, Game
    options, Graphics page, Achievements, Key card, Change keys, Quit to desktop; hazard-tape look; pause when the window
    goes to the background), `PS2_PERF_LOG` (a performance summary per run), and the widescreen fix: `--widescreen` no longer
    stands in for recomp.ini's widescreen option (it also set the console's 16:9 setting), the window opens in the chosen
    shape and changes shape with the option.
  - `0006-achievement-popup.patch`: the achievement pop-up with its picture (`PS2_ACHIEVEMENTS_ICONS=<folder>`, `<id>.bmp`
    read with SDL_LoadBMP), bottom right, in the menu's hazard look.
  - `0007-badges-master-volume.patch`: badges on the in-game Achievements page (`<id>.bmp` in colour once unlocked,
    `<id>_lock.bmp` grey until then; grey made from the colour one when there is no lock picture), a points bar and the
    highlighted achievement's description and unlock time; the Graphics page becomes *Graphics and sound* with a master
    volume (keys.ini `master_volume`, 0-100, applied in the sound callback); widescreen is listed there only (no longer
    also on the Game options page).
  - `0008-picture-settings-lens-rain.patch`: the Graphics and sound page reworked: *Internal resolution* (1x-4x or `scale = auto`,
    chosen at the start from the screen height), *Upscale filter* (the looks, renamed; **Raw pixels** is the default now),
    *Whole-number scaling* (keys.ini `integer_scale`, video option bit 3, `picture_view` in gpu.cpp), *Rain on the lens*
    (keys.ini `lens_rain`; host/lens_rain.cpp simulates the drops, `ps2::set_lens_drops` hands them to blit.wgsl's
    `drops_over`, binding 2; `PS2_LENS_WHEN=ADDRESS=VALUE` limits them to when the game is playing).
  - `0009-smaa-antialiasing.patch`: SMAA 1x (a port of the MIT reference to WebGPU compute passes, runtime/src/gs/gpu/smaa.wgsl,
    tables in third_party/smaa) on the console picture before it is enlarged; F1 -> Graphics and sound -> *Anti-aliasing*
    (off / smooth edges / SMAA 1x, keys.ini `smaa`), video option bit 4.
  - `0010-fsr1-upscale-filter.patch`: FSR 1 as a fourth upscale filter (EASU, the edge-adaptive enlargement of AMD FidelityFX
    Super Resolution 1, MIT, written out in runtime/src/gs/gpu/blit.wgsl; RCAS left out because the picture is drawn straight to
    the window). Looks 6 and 7 (`look = fsr` / `fsr_edges`, video option bits 1 and 2 together), F1 -> Graphics and sound ->
    *Upscale filter*, F2.
  - `0011-disc-remap-slots.patch`: up to 8 disc remaps at once (one per recomp.ini section), and `remap_on = FROM TO COUNT`
    (remap while the option is on; `remap_off` stays for the voices). The Enhanced Edition's Japanese textures use it:
    F1 -> Game options -> *Japanese faces and hair*, *Japanese signs and packets*.
  - `0012-build-jobs-everywhere.patch`: the translator and unpacker builds in port.sh also take `PS2_JOBS` (the launcher sets
    it: fewer compilers at below-normal priority so the PC stays usable while building).
  - `0013-language-switch-entry.patch`: F1 -> *Language* (PS2_LANGUAGE, PS2_LANGUAGE_OTHER, PS2_LANGUAGE_SWITCH_FILE from the
    launcher): writes the file, writes the memory card, ends the game; the launcher starts the other build.
  - `0014-menu-skin.patch`: the F1 menu's Japanese skin (blue on white) next to the hazard one; Graphics and sound -> *Menu
    look*, keys.ini `menu_skin`, PS2_MENU_SKIN from the launcher. Also: only the highlighted entry shows "Enter again".
  - `0015-settings-tabs-key-prompts.patch`: F1 -> Settings in tabs (Display, Picture, Sound, Controls, Game; window size
    presets, the game's blur filter switch, reverb switch); PC keys in the game's font (PS2_PROMPT_FONT_KEY, PS2_PROMPT_CELLS,
    `ps2::set_button_prompts`, with `tools\rd_prompts.py`); key caps on the Change keys page; lens rain follows the map's
    weather (PS2_LENS_WEATHER); texture overrides sized to the device's largest buffer binding.
  - `0016-native-options-mouse-look.patch`: PC settings from the game's own Options screen (PS2_NATIVE_MENU, Select), drawn in
    that screen's look with the game's font (PS2_MENU_FONT); a third menu look (`menu_skin = game`). Mouse look
    (PS2_MOUSE_LOOK_YAW / _PITCH / _WHEN): right stick pushed just enough, the game's camera angle read back every frame;
    Settings > Controls: mouse look, mouse speed, mouse up and down.
  - `0017-live-resolution-smaa-presets-key-art.patch`: inner resolution 1x-6x and 8x and the 2D art filter changed while the game
    runs (memory read back, buffers and programs made again); SMAA presets low/medium/high/ultra; key caps in the game's own
    style (rounded outline, the game's letters, 2x font); mouse look open loop per game step, modern direction, invert switches,
    look_speed 2.0; lens drops slant with the camera's turning (PS2_LENS_YAW).
  - `0018-native-options-line-jp-live-walk-speed.patch`: the game's own Options list gets a PC settings line in place of "Adjust
    screen position" (PS2_NATIVE_ROW: the list's table pointer finds the game's cursor, the help line is rewritten in the game's
    text, Cross opens the PC settings in the screen's look); the Japanese pictures (overrides under jp/) switch at once with
    the in-game switch (ps2::set_jp_textures); readable key names (LMB, RMB, Spc, Ent...), key caps sized to the game's font,
    no key card; F6 held: fast forward; the mouse wheel sets the walk keys' speed (5 steps).
  - `0019-bars-rain-under-hud-sharpen-map-select.patch`: cutscene black bars left out on request (gs.cpp, cinema_bar: two
    untextured opaque black sprites at the screen's top and bottom edge; keys.ini black_bars); the boxes of the game's 2D
    (gs.cpp, ui: what is drawn after the full-screen passes, or at the front of the depth range) handed to blit.wgsl so the
    lens rain stays under the HUD, texts and bars; contrast-adaptive sharpening in blit.wgsl (video option bits 5-6, keys.ini
    sharpen); F7 / Settings > Game: the developers' map select (PS2_DEBUG_SCENES, the game's next-scene word); the card check
    at start answers itself (PS2_AUTO_CROSS); F1-F9 are the program's keys.
  - `0020-crisp-copies-bar-opacity-banks-card-skip.patch`: **crisp picture at every internal resolution**: the game copies
    each finished picture into the shown buffer at 449 rows from 448, and the engine read such near 1:1 copies of a rendered
    picture with the console's one-texel tent (gs.wgsl sample_scaled), a blur of about one console pixel whatever the scale;
    `PS2_RT_COPY_TENT=0` (pipeline constant RT_TENT) reads them by their own inner samples. Cutscene bars by opacity (0-100),
    the subtitles' halo fading with them (gs.cpp subtitle_shade); the lens mist no longer cut at the HUD boxes; the card
    check run unseen at full speed (`PS2_SKIP_SCENE`, blit option bit 7: black); a second disc remap per recomp.ini option
    (`remap2_off`/`remap2_on`, slots from the other half); F7 retries for 3 s and opens only from the scene given.
  - `0021-native-option-rows-host-calls.patch`: **PC rows inside the game's own Options list** (title and in-game menu):
    Resolution, Anti-aliasing, Cutscene bars, Lens rain, Mouse look. The game's list engine (OPTION.BIN) draws them, moves its
    cursor over them, takes Left / Right and shows their help line; the host only builds the row table (in memory the game
    never uses, 0x80000) and points the list at it. Each row's get / set is a **host function the game calls**: a new engine
    feature, `ps2::register_host_call` (generated.hpp): guest addresses 0x0FF00000 + 4 * n run host code and return like a
    guest function (dispatch.cpp). The rows' words are pictures painted into the game's own label sheet
    (tools/rd_options.py, a texture override of its 512 x 512 upload); their sprite entries are reached through the RAM's
    mirror at 32 MB (the game skips negative sprite indexes).
- **Upstream checks are off.** From v0.7.4 the launcher no longer compares this fork with upstream at start (`EngineCheck2=1`
  turns it back on): fixes for Raw Danger are made here, in our own patches, not waited for from upstream.
- Game fixes live in the patch files (`patches\pnach\`), not in the engine: the fast loading fix, widescreen.
- **0022-options-list-scroll-overrides-per-look-smaa-search-menu-runs-rain** (rawdanger-recomp v0.7.7): the game's Options list
  gets every PC setting as rows of its own (settings with more than two values are spinners "< value >": the game moves to the
  first or third value, the host steps the setting and the game is back on the middle one), the live recomp.ini options, eleven
  rows at a time with scrolling and scroll arrows (the game's table is the rows on screen, built again as the cursor moves; the
  records are written word by word only where they differ, as the game draws from them meanwhile); the page's fixed bars are at
  0x73D3D0 (0021 looked 4 bytes early, so they showed between the rows). Texture overrides: a texture listed with several
  pictures takes the first one of the look in force (pictures under jp/ while the Japanese look is on, the others otherwise),
  each decoded once; before, the last line decided and the Options sheet with the PC words was dropped whenever the Japanese
  list also had it (the words then came from the sheet's empty padding: noise). SMAA: SearchTex read with point filtering, as
  the reference binds it. keys.ini menu_game_runs (on by default): the game goes on behind the F1 menu (it still waits when the
  window goes to the background with pause_unfocused on). Rain on the lens: one on/off setting (rain_on_lens, on by default;
  the old lens_rain line is not read), its strength from the map's rain, eased in and out, with random gusts.
- **0023-game-colour-filter-one-pass** (rawdanger-recomp v0.7.8): Raw Danger's colour filter (its brightness tint and depth
  haze, after the bloom: the frame walked one 64 x 32 page at a time, about 2,800 small draws a frame each reading what the
  one before wrote) recognised in the GS stream (the first page of the depth copied as 16-bit into a 64-wide buffer) and done
  in one pass: the walk's draws are left out, their palettes and blend weights noted as they pass, and the formula runs over
  the frame at the end of the walk (software renderer: rdfilter in gs.cpp; GPU: the game_filter pass in gs.wgsl, every sample
  at any internal resolution). The same picture as the game's own draws to the last bit (software renderer, GPU at 1x and 2x);
  GPU batches a frame 2,065 -> 105. `PS2_GAME_FILTER_PASS=1`.
- **0024-developers-text-sjis-and-size** (rawdanger-recomp v0.7.8): `PS2_SJIS_TEXT=<texture base>`: text that a program prints
  as Shift-JIS byte by byte through an ASCII font sheet (Raw Danger's scene select, DEBSEL) is shown in ASCII (the lead byte's
  sprite left out, the trail's moved to the same character's ASCII cell, packed along its line); and, while the window says a
  developers' scene is on screen (ps2::set_debug_text), that sheet's 24-pixel letters stretched to 32 are drawn at their own size.
- **0025-direct-mouse-look** (rawdanger-recomp v0.7.9): `PS2_MOUSE_LOOK_DIRECT="WORD,CHECK=VALUE"`: with a code patch in the
  game that adds the float at WORD to its camera's turn of each step and clears it (Raw Danger: the pnach section [Direct mouse
  look], 0x7F10, the build checked at 0x176FD0), the mouse's turn is put there whenever the game has taken the last one: as
  far as the mouse moved, in one step, no top speed. A camera that leaves the word unread for two steps gets the stick push as
  before; a -0.0 probe tells when it reads it again.
- **0026-v0.8.0-menus-popups-keys-mouse** (rawdanger-recomp v0.8.0): the texture picker falls back to the ordinary picture when
  the Japanese look has none (the Options background's words); kept pictures are forgotten when the override space wraps (the
  garbled cut-out sprites after a texture switch); the F1 menu in the Options screen look by default, a bigger achievements
  page, `popup_size` (small / normal / large / huge) and `keys_on_screen` (F10) in keys.ini; direct mouse look counts unread
  time in console frames (`ps2::frame_count`), so it no longer flips to the stick at 144 Hz or on a hitch; walk step 6 holds
  Cross (the game's run); the texts' key caps follow the input last used; the post processing switch renamed.
- **0027-v0.8.1-filter-replay-texture-switch-arrows-icons-rain** (rawdanger-recomp v0.8.1): the colour filter's first page is
  drawn as the game draws it while its steps are written down, then replayed for the rest in one pass (two or three depth
  palettes, writes from palettes or a flat colour; anything else is left to the game's draws); `ps2::set_game_filter_pass` and
  keys.ini `post_processing` to compare; texture options switched in an area wait for the area load (`PS2_AREA_LOAD`); built-in
  fixes hidden; left / right and clickable arrows on the game's options; the window icon (`PS2_WINDOW_ICON`, resource 1) and
  `PS2_EXE_ICON` built into ps2run.exe; the lens rain follows the player's movement (`PS2_LENS_MOTION`) with a clearer drop.
- **0028-v0.8.2-ambient-occlusion** (rawdanger-recomp v0.8.2): `gs.wgsl` game_ao, run at the start of Raw Danger's colour filter
  on the 3D picture from the game's depth (pairs of samples: creases only), `ps2::set_ambient_occlusion`, keys.ini
  `ambient_occlusion`, F1 > Picture.
- **0029-v0.8.3-fxaa-ao-rain-mouse** (rawdanger-recomp v0.8.3): FXAA 3.11 in `smaa.wgsl` (quality 8, keys.ini `smaa = fxaa`),
  stronger ambient occlusion, the lens rain turns with the camera's view direction and leaves short marks, `look_speed` 1.0.
- **0030-v0.8.4-filter-variant-ao-walk-run** (rawdanger-recomp v0.8.4): the filter replay accepts a walk with no brightness
  stage (AREA 01's streets), ambient occlusion with fixed samples, the mouse wheel switches walk / run.
- **0031-v0.8.5-filter-camera-vu1-stats** (rawdanger-recomp v0.8.5): the filter replay keeps eight writes, PS2_CAMERA_PITCH
  reads the camera block at a fixed address ("@"), the lens rain gets heavier looking up, PS2_VU1_STATS counts VU1 programs.
- **0032-v0.8.6-fov-debug-wording** (rawdanger-recomp v0.8.6): keys.ini field_of_view through the camera's zoom factor
  (PS2_CAMERA_ZOOM), FXAA in the Options list, the filter's name in sentence case, the debug menu notes, the UNPACKs in PS2_VU1_STATS.
- **0033-v0.8.7-ao-debug-menu** (rawdanger-recomp v0.8.7): stronger ambient occlusion; the developers' debug menu drawn in
  the PC font with its cursor followed from the pad; a menu canvas fill wholly off its picture no longer writes past it.
- **0034-card-check-frames-ao-metres-texture-switch** (rawdanger-recomp v0.8.9): PS2_AUTO_CROSS answered in the game's own
  frames by ps2::latch_pads (it missed when timed by the window); ambient occlusion measured in metres (no far outlines, no near
  smears); every texture decoded again when the regional textures switch while playing.

## Goals of the fork

1. **Look like the real console.** Resolution x1 and "Text and menus: exact" in the F1 menu give the PS2's own picture;
   the game's post-processing filter stays on (the GPU path draws it exactly, so it is not removed).
2. **Run faster** without changing what the game does: measured first (`Performance report` in the launcher writes
   `PS2_HITCH_LOG` / `PS2_GPU_STAT` logs into `games\<name>\logs\`), then fixed where the time goes.

## Updating from upstream

Replace the folder with the new upstream tree, `git apply` every patch in `patches\native\` in order, build one game.
