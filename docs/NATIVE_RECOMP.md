# Moving Raw Danger! to ps2-native-recomp

Status: **playable** (2026-11-12, Léo: app build with Widescreen 16:9 + Fast loading fix, opening, intro text, the hotel party gameplay at ~60 fps). Open: overlay cache limit reached (`overlays\` 8 per window / 32 total), so later areas pause briefly the first time they are translated; rebuilding bakes the recorded code in. JP disc and the demo not built yet.

History: **opening plays** after the fix below. First native build (2026-11-12): boots with sound and the FMV decoder working,
crisp picture, ~54-60 fps, menus and character select fine, then it **stayed on the GAMELOAD screen** (black, RAW DANGER
logo, blue bar). Found by replaying the F9 save state on a Linux build of ps2-native-recomp (same commit 0cc2658):

- GAMELOAD (scene table 0x3A7FF0, init 0x181E80) runs the state chain 0x181E80 -> 0x181E20 -> 0x181DD0 -> 0x181D80 ->
  0x181D20 and waits there for the byte at gp-0x4940 (0x48F530). The loading-screen task (init 0x185A10, states
  0x185880 -> 0x185700 -> 0x1856C0 ...) sets it when it is done, and it first waits for its 10-step fade-in to end
  (fade state 0x6A4198 == 3, updated once per main-loop pass by 0x2C76A0).
- ps2-native-recomp answers FILEIO reads at once (sif.cpp fileio_async, no disc time), so the whole map (8.5 MB
  M1_00_00.DAT) loads in ~16 frames while the main loop, at 20 passes a second, has run fewer than 10 fade steps. The
  post-load function 0x11AA60 then resets the fade (jal 0x2C77E0 at 0x11B2B8): state 0, never 3, the loading screen
  waits for ever and so does GAMELOAD. On a PS2 the load takes seconds and the fade always ends first.
- Fix: `[Fast loading fix]` in `patches/pnach/SLUS-21501_2905C5C6.pnach` NOPs that jal (JP: 0x11B248, matched from the
  US code, not yet run). Checked: from the F9 state the game goes on to SCRIPT_KICK(Shinohara_Opening) at frame 7868 and
  the opening plays with textures and subtitles ("IREM Software Engineering Inc. Presents", the Geo City intro).
- The other way (make FILEIO reads take disc time in the runtime) would also fix it and is closer to a PS2, but every
  load gets slower; the patch keeps the fast loads.
- Developer extras: `patches/native/0001-pnr-game-printf-and-sifdma-trace.patch` (runtime: syscall 0x7D prints the
  game's own printf, PS2_TRACE_SIFDMA=1 prints SIF DMA payloads) + `SLUS-21501_2905C5C6.debug.pnach` [Game debug print].
  Useful tools of the runtime itself: --load-state report.state, --press F:button, --save-state F:file, PS2_WATCH=addr,len,
  PS2_EE_LIVE=1 / PS2_VU_LIVE=1 for headless runs, --frame-every N --frames-dir D.

Notes from other PS2 tools:
- PCSX2 GameIndex (SLUS-21501, SLES-54587, SLPS-25606): compat 5, and only GS hardware-renderer fixes (half-pixel
  offset, texture-inside-RT, auto flush, ...). No EE/IOP/VU timing or clamping fixes, so the game is not known to
  depend on cycle-exact timing - a hang points at a runtime gap, not at the one-cycle-per-instruction model.
- ps2-native-recomp keeps at most 8 translated overlay images per window address and 32 in all (16 MiB),
  `runtime/src/ee_live.cpp` kOverlaysPerBase/kOverlaysTotal/kOverlayBytes (PS2_EE_LIVE_OVERLAYS can only lower it).
  Raw Danger loads ~60 map overlays at the same addresses, so a later rebuild cannot keep all of them; raising those
  limits is the first game-specific tuning to try once the game gets past loading.

## Why

ps2recomp (ran-j/ps2recomp) recompiles the EE code, but its README lists the limits we kept running into: "Performance
is very bad for VU and GS" and "Hardware emulation is partial and many paths are stubbed". In this project that meant
fixing the console piece by piece, game-side:

| What we had to build or fix on ps2recomp | ps2-native-recomp |
|---|---|
| HLE for the IOP: pad (libpad2), SPU sample uploads answered on the EE side, sceCdSearchFile routed by hand | the game's own IOP modules run on an interpreted IOP against rebuilt hardware |
| No sound (no SPU2), FMVs skipped (no MPEG decoder) | SPU2 (2 cores, 48 voices, reverb) and the IPU MPEG decoder are rebuilt |
| CPU rasterizer, ~31k sprites/frame for the post filter, very slow | GS as compute shaders on the GPU (D3D12 / Vulkan), 2x/3x resolution |
| libgraph clear/double-buffer stubs (patch 0015) | no libgraph stubs: the kernel is rebuilt, the game's own libgraph runs |
| DMA SPR bit (0016), VU0 macro memory (0017) translator/runtime bugs | 54/55 ps2autotests match a real console |
| 79 overlays found and recompiled ahead of time (rd_mkconfig overlay pass) | code the build missed is translated during play and cached |
| Guest memory layout patch (0003/0004) so the game's heap fits | the kernel and runtime don't live in guest RAM |

It has no game-specific code: 53 games tested, 34 out of the box. Known limits (its README): slower than PCSX2 (GS
compute), one cycle per instruction, patches written into memory at run time don't change translated code, Windows 10
not checked by the author (Léo's PC is Windows 10 22H2).

## What carries over from this repo

- **Patches** (`patches/patchdb.json`) -> `patches/pnach/SLUS-21501_2905C5C6.pnach`, checked with ps2-native-recomp's
  `pnach.py sections/apply` against the retail ELF: [Widescreen 16:9] (3 sites, same as PCSX2's file) and
  [No post-processing filter] (optional). PCSX2's own file for this CRC exists and the app finds it by itself.
- **Disc facts**: US ELF `SLUS_215.01`, PCSX2 CRC `2905C5C6`; JP `SLPS_256.06`, CRC `C988ECBB`. Data in `ROOT.DAT/FAT`,
  voices and films in `STR\STR.DAT/FAT`.
- **Voice swap (US/JP)**: `rd.py voicepack` builds the hybrid `STR.DAT/STR.FAT`. On ps2-native-recomp the disc image is
  read directly, so the swap becomes a rebuilt disc image with those two files replaced (its `scripts/mkiso.py`, or any
  ISO tool), then built like any disc. Not done yet.
- **Game knowledge for debugging** (`docs/WORKFLOW.md`): scene/script names, the render command interpreter (0x30FCE0),
  the map-matrix palette in VU0 memory, texture loader (TexGetTEXimage 0x2D6A50). Useful if something renders wrong
  there too.
- **Roadmap features** (QoL, Recomp Settings menu, keyboard+mouse, JP textures, packs): several already exist there
  (quick saves F5/F8, key remapping F1, picture styles F2, 2x resolution, widescreen patches). The rest would be built on
  top of it instead of on ps2recomp.

Not carried over: the ps2recomp runtime patches 0001-0017 and the HLE in `recomp/game/` - they fix ps2recomp's runtime,
which ps2-native-recomp replaces.

## First test (Léo, Windows)

1. Download https://github.com/exoteric33/ps2-native-recomp (Code -> Download ZIP), unpack to a short path, e.g.
   `C:\PS2Native` (long paths break the build).
2. Double-click `Start.cmd`, press *Run* when Windows asks.
3. *Add a game* -> `C:\Users\ramos\Desktop\RAW DANGER DECOMP\GAME US\Raw Danger (USA).iso`.
4. *Choose extras...* -> tick Widescreen 16:9 if you want it (or leave everything off for the first try).
5. *Build* (first time downloads ~312 MB of tools, then several minutes), then *Play*.
6. Play through boot, menus, character select and the opening (Shinohara_Opening). Press **F9** wherever something
   looks or sounds wrong: it saves a report under `reports\` with `log.txt` and `info.txt`.
7. Copy the newest `reports\` folder(s) and, if the build fails, the build log into
   `RAW DANGER DECOMP\native_test\`, then "check folder".

Keys there: F1 key card, F2 picture styles, F5/F8 quick save/load, F11 fullscreen, Esc = Start, WASD = left stick.
`PS2_SCALE=2` before starting = 2x resolution.
