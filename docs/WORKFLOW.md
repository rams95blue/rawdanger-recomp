# Workflow — from disc to running recomp

Each step says **what** to run and **why** it matters, so the
reverse-engineering side stays understandable.

## 1. Extract both discs — `rd extract`

`tools/rd_iso.py` reads the ISO9660 file system directly (no 7-Zip needed) and
writes `work/<region>/_rd_manifest.json` with every file's **LBA** (start
sector), size and CRC32.

*Why LBAs matter:* many PS2 games skip the file system and read
`sceCdRead(lba, sectors)` using a table of sector numbers baked into the
executable. If Raw Danger does that, the recomp must see the **original disc
layout**, so we mount the ISO (`[disc] image`). The ps2recomp runtime serves
name-based opens from the extracted folder using fake LBAs at ≥ 0x100000; the US
disc ends well below that (~791k sectors), so the two can't collide. The JP ISO
is bigger than 2 GB and *can* collide, which is one more reason the build
targets the US ELF.

## 2. Measure the versions — `rd compare`

`tools/rd_scan.py` scans every byte of both trees for known PS2 asset headers,
including headers buried inside Irem's archives:

| signature | what it tells us |
|---|---|
| `VAGp` | SPU-ADPCM sample → sample rate, duration |
| `SShd` | SS2/ADS stream → rate, channels, PCM vs ADPCM |
| `IECSsreV` | HD sound bank (SFX/voice bank header) |
| `TIM2` | texture → size, bit depth, mipmaps |
| MPEG `00 00 01 B3` | FMV → resolution, declared bitrate |
| `MWo3` | Metrowerks overlay — **code** loaded at runtime |
| `\x7fELF` | IRX / ELF modules |

It also samples each file's zlib ratio (a rough "how compressed is this
already" measure). `report.md` opens with a measured verdict per category:
higher sample rate, larger textures, higher video bitrate. Then it lists every
file that differs, US-only and JP-only files, the raw MWo3 header words, and
voice-swap candidates.

> The JP ISO is 2.31 GB vs 1.62 GB for the US one. The report will say where
> the extra ~690 MB is: higher-rate audio, extra voice lines (JP NPCs speak the
> protagonists' names), FMV bitrate, or plain padding.

## 3. Analyze the ELF — `rd analyze`

`tools/rd_elf.py info` reports segments/sections, the **file CRC32** (the key
ps2recomp game overrides match on — this is *not* the same number as PCSX2's
`2905C5C6`), SDK library versions (`PsIIlib…`), the IRX modules the game loads
(these run unmodified on ps2recomp's IOP emulator), and the **LBA-table
heuristic**: it looks for each disc file's start sector inside the ELF, next to
its size or sector count. Several paired hits mean there's a hard-coded TOC.

`rd_elf.py verify` checks every site in `patches/patchdb.json` against your
ELF: is it code or data, and does the current value match the expected
original? A mismatch means a different build, and the patch is refused.

## 4. Function boundaries — `rd ghidra` (recommended)

ps2recomp's README recommends a Ghidra function export over its built-in
analyzer for retail/stripped games, and on this ELF the difference is large:
the largest generated function drops from 0.9 MB (analyzer) to ~0.1 MB.

One-time setup:
- Ghidra 12.2 (needs **JDK 25**) anywhere next to this project, e.g.
  `RAW DANGER DECOMP\gihdra+ps2engine\ghidra_12.2_PUBLIC_…\ghidra_12.2_PUBLIC\`
- *ghidra-emotionengine-reloaded* built for the same version, installed via
  File → Install Extensions (it lands in `%APPDATA%\ghidra\ghidra_12.2_PUBLIC\Extensions`)

Then `rd ghidra`:
1. finds `support\analyzeHeadless.bat` up to 4 folders deep next to the project
   (or `--ghidra <dir>` / `GHIDRA_INSTALL_DIR`), and checks the PS2 extension is installed,
2. imports `work\us\SLUS_215.01` as **`r5900:LE:32:default`** into `work\ghidra\project\`
   and auto-analyzes it,
3. runs ps2recomp's `ExportPS2Functions.java` through `tools/ghidra/RdExportPS2.java`,
   a subclass that answers the script's two Save dialogs from arguments, so no GUI is needed,
4. writes `work\ghidra\SLUS_215.01.toml` + `.csv`.

Running it again re-exports from the saved project without re-analysing;
`--force` re-imports. You can open the same project in the Ghidra GUI to
explore (File → Open Project → `work\ghidra\project\RawDanger.gpr`).

`rd config` also always runs `ps2_analyzer`. Ghidra gives good function
**boundaries**, but on a stripped ELF it can only name kernel-call wrappers (58
here). The analyzer matches ps2recomp's SCE library symbol database and names
~200 SDK functions (`sceSifInitRpc@0x002B5EA0`, `sceCdInit@0x002BFEE0`, …). Those
are merged in as stubs, so they run as the runtime's built-in handlers instead
of the game's own copies. Without this, the first boot spins forever inside
`sceSifInitRpc` waiting for an IOP register (PC=0x2B5FF8).

`rd config` then **cleans** the export against the measured code ranges.
Entries that start in data or at unaligned addresses are dropped, and function
ends are clamped at the code boundary. Bad boundaries would otherwise make
ps2recomp fail ("Unaligned function table entry") or emit huge fake functions.

Worth looking at in the GUI:
- the code around `0x0011E104` (the post-filter call sites `no_post_filter` NOPs out),
- callers of `sceCdRead` / the `STR.FAT` lookup (voice streaming),
- the `MWo3` overlay loader (ROADMAP phase 2).

## 5. Generate config — `rd config`

`tools/rd_mkconfig.py` keeps everything from the base TOML (stubs,
entry_points, jump tables, mmio) and adds:

- `general.input` / `output` → your ELF / `ps2xRuntime/src/runner`
- `[patches] instructions` for **code** patch sites of the selected groups
- `recomp/game/rd_build_info.h` (ELF name, CRC32, entry), so the game module
  only activates on your exact build
- `recomp/game/rd_patch_table.inc`: **data** patch sites, toggled at boot from the INI

Pick code patches with `--code-patches widescreen,no_post_filter` (or `none`).

## 6. Recompile & build — `rd recomp`, `rd build`

`rd recomp` clears stale generated files, runs `ps2_recomp`, then copies the
game module (`recomp/game/*`) into the runner folder.

**Data compiled as code.** Raw Danger's ELF has one writable+executable
segment holding code *and* data (floats, tables), and no section boundaries.
So the recompiler happily "recompiles" data into gigantic fake functions:
13.5 MB of C++ for one of them, which drove MSVC to 58 GB and C1002.
`rd_elf.py coderanges` finds the real code from instruction statistics:
**0x100000–0x34FE44** plus a small island at 0x43A000–0x43C0A8 (US). `rd recomp`
then skips every generated function that starts in data, writes them to
`work/data_skip.txt`, and recompiles. Later runs are a single pass. Largest
generated function on the real US ELF: 0.9 MB. ps2recomp's CMake
compiles every `.cpp` in that folder into `ps2EntryRunner`.

`rd build` builds Release with aggressive logs off and **whole-program
optimization off**: `/GL` + `/LTCG` over ~1M recompiled instructions exhausts
MSVC's heap (C1002 "out of heap space in pass 2" / LNK1257) even on 64 GB.
`patches/ps2recomp/0002-rawdanger-optional-ltcg.patch` adds the
`PS2X_ENABLE_LTCG` switch, and `rd build` applies any missing patches itself.
Flags: `--ltcg` (turn it back on), `--unity-batch N` (fewer generated files per
compile unit = less RAM per compile), `--jobs N`,
`--debug` (Debug + verbose logs), `--debug-ui` (ImGui debugger panel, good for
the first boots), `--no-ffmpeg` (no FFmpeg DLLs, but FMVs show stub frames).

## 7. First boot — the iteration loop

ps2recomp's recommended loop, applied here:

1. `rd build --debug --debug-ui`, then `rd run`.
2. Fix hard blockers first: `function not found`, syscall TODOs, `sceCdRead unresolved LBN`.
3. Use temporary `ret0@0xADDR` stubs only to learn which calls matter.
4. Replace those with real implementations.
5. Put game-specific hacks in `recomp/game/rd_game.cpp`, not in ps2recomp.
6. Retest from a cold boot after each batch.

### Debugging aids in the game module (no re-recompile needed)

- **Game debug printf.** Irem compiled their debug `printf` down to an empty
  function (US `0x2C6240`). The module rebinds it to a formatter that reads the
  EE argument registers, so the game's own diagnostics appear as `[game] …`:
  `FatMap: FatData = …`, `FatSearchFile: Not Found …`, `HfileReadAlloc: Read Error…`.
  `[debug] game_printf = off` disables it.
- **Tracer.** `rd run --trace 0x2C9DD0,0x2C8B70` (or `[debug] trace =`, or
  `RD_TRACE`) wraps up to 16 game functions and logs `a0..a3` (with strings or
  the first 16 bytes behind pointers) and `v0`. Repeated lines are rate-limited
  (first 40, then every 5000th with a count). Suffix `!z` (`0x2D6A50!z`) logs
  only calls that return 0 - failed lookups - with their arguments and caller.
  Suffix `!m` (`0x3109A0!m`) dumps the 4x4 float matrices behind a0/a1/a2 before
  the call and behind a0 after it.
  The game's own printf is rate-limited the same way, so a message seen 40 times
  may have happened thousands of times.
- Thread watchdog: `rd run --threads 5` (or `[debug] thread_dump = 5`, env
  `RD_THREADS`) logs the EE scheduler snapshot every 5 s: every guest thread's
  state, what it waits on (sema/evflag/vsync/...), PC/RA, and semaphores /
  event flags with waiters, flagging `NO PROGRESS` when the EE cycle counter
  stops. Use it when the game goes quiet without an error.
- **GS capture (F9).** Press F9 in the game window: the GS frontend (patch
  0010) records the next `[debug] gs_capture_frames` (default 2) presented frames
  to `work/us/gs_captures/cap_<date>_<time>.gsr`: register state and the 4 MiB GS
  local memory at the first frame boundary, then every GIF packet, image
  upload, register write and the display registers at each frame. A few MB per
  frame. `tools/gs_replay` (`gs_replay cap.gsr outdir --vram --log`) renders it
  offline with the same GS code: `frame_NNN.png`, local memory as images, and
  `draws.txt` with every draw (primitive, frame/texture buffer, formats, CLUT,
  alpha/test, bounds). Renderer bugs are reproduced and fixed from the capture,
  without the game or the disc. `rd run --gscap 40` (or `[debug] gs_capture_at`)
  starts one capture automatically 40 s after boot instead of F9. F9 is read with
  IsKeyDown edge detection on the game thread (IsKeyPressed is only true for one
  render-thread frame and was missed).
  F9 while a capture runs stops it at the next frame; `rd run --gscap 1
  --gscap-frames 100000` records from boot until F9. `gs_replay ... --every N`
  writes only every Nth frame PNG; `draws.txt` marks `# record N` per packet.
- Both use `PS2Runtime::replaceFunction` on the dispatch table, so after
  editing `recomp/game/*` just run `rd build` (it syncs the game module
  first): only the game module's compile unit rebuilds.

Boot findings so far (US):

| PC | what | status |
|---|---|---|
| 0x2B5FF8 | `sceSifInitRpc` spinning on an IOP register | fixed by merging analyzer SDK stubs |
| 0x2D23E0 | IRX loader spins: `FatSearchFile` gets a NULL table because `HfileReadAlloc` couldn't allocate 281,376 bytes (`MemutilAllocAlign: Alloc Error`) | fixed by the guest memory layout below (patches 0003 + 0004) |
| 0x2D2498 | `Can't Load Module IOP\IOP.DAT`: the game DMAs each IRX into IOP RAM and calls `sceSifLoadModuleBuffer(iopAddr)`, but ps2xIOP looked for the ELF in EE RAM | fixed by patch 0005 (fall back to IOP RAM); all 14 IRX load and start in the real game |
| 0x2BF758 | Spins after `fileReadFat: ROOT.FAT`: the game's own `sceCdLayerSearchFile` and its `sceCdSearchFile` wrapper (0x2BF9B8, unnamed by the analyzer) were recompiled as plain code and wait for CDVDFSV RPC 0x80000597, which only exists in the real IOP BIOS | `rd_game.cpp` routes both to the runtime's `sceCdSearchFile` (`[hle]` in rawdanger.ini); ROOT/STR archives open |
| 0x1AFDC0 | Hangs after `VU1work Adr = 700006A0`, inside libgcc soft-float `_fpmul_parts`: ps2recomp's `PS2_PMFHL_LW` copied whole 64-bit LO/HI lanes instead of interleaving their 32-bit words, so `multu; pmfhl.lw` lost the top half of every 64-bit product and the normalise loop spun on a zero mantissa | patch 0006 rewrites all five PMFHL variants to EE/PCSX2 semantics (200k-case reference test, 0 mismatches). It edits a precompiled-header file, so the next `rd build` recompiles everything once |
| 0x2C9824 | Black screen after game init (items, task groups, `se\common.hd` loaded to IOP 0x120000): the EE retries IREMSND2 command 0x90001100 (`FUN_00309e50`, RPC 0x12346) forever. IREMSND2 streams sound banks with ioman `open`/`lseek`/`read`/`close`, which ps2xIOP didn't implement: `open` "succeeded" with fd 0 and `read` returned 0 bytes, so its chunk loop never ended (2 million warnings, 155 MB log) | patch 0007 implements read-only ioman file I/O over the IOP host file API (+ a test of IREMSND2's exact call pattern) and rate-limits unhandled-import warnings |
| 0x2C9504 | `FileReadAllocSPU` (`se\common.bd`, 260,480 bytes to SPU address 0x5000) retries forever, leaking one ROOT.DAT descriptor per try. IREMSND2 init starts looping PCM auto-DMA (`sceSdBlockTrans(core, WRITE\|LOOP)`) on both SPU2 cores; the bank upload then calls `sceSdVoiceTrans` on core 1, which LIBSD refuses with -210 (ADMAS set = channel busy). Reproduced in a ps2xIOP harness with the real IRX (init + upload). | The game's real command sequence (init, 0x4000, 0x80000110/111/120, 0x80000100, 0x20000410/0x80005070 polls, 0x80005030/80, 0xFF0) replayed in the harness gives identical results and still fails: no command stops core 1's stream. LIBSD's check matches PCSX2's register model, so the hardware difference is still unknown. Since ps2recomp has no SPU2 audio yet, `rd_game.cpp` answers command 0x90001000 on the EE side with the success size (`[hle] spu_bank_upload`, logged). Real fix belongs to the audio work (ROADMAP phase 3b) |
| 0x312940 | After `BOOT.DAT` and the first map models load: `guest-branch:missing-target` on a `jalr` to 0x312940, a 2-instruction thunk (`j 0x312720`) that is only referenced from a function-pointer table in data, so Ghidra never made it a function. The ELF has ~194 such pointer-only targets (vtable methods, callback tables, `jr ra` stubs) | `rd config` now adds them: `add_pointer_targets` in tools/rd_mkconfig.py scans data for aligned code pointers into gaps between known functions and appends `ptr_XXXXXXXX` entries to functions.clean.csv (gaps over 16 KB are skipped and listed). Verified with ps2_recomp: `ptr_00312940` tail-calls `FUN_00312720` and is registered. Needs `rd config` + `rd recomp` + a full `rd build` |
| 0x738DE0 | After timetable + item-menu textures load and `OVERLAY OVERLAY\DISC\MCMENU.BIN is Loaded`: `missing-target` on a `jal` into the memory-card-menu overlay. Overlays are not recompiled yet (ROADMAP phase 2) | `rd overlays` extracts the 79 MWo3 files and recompiles each at its load address (tools/rd_overlay.py: synthetic ELF per overlay, own function discovery, names prefixed `ov_<NAME>_`); patch 0008 lets the runtime hold functions outside the main table; `rd_game.cpp` wraps the post-load routine `FUN_00100290` and swaps the overlay's entry points in (name + address + size + CRC32 checked) before its constructors run |
| 0x182240 | Overlays work (MCMENU registered 207/207, its code loads `MCMENU\MCMENU.DAT`); next: `missing-target` on a `jalr` to 0x182240, a `j 0x16ED80` thunk whose address the neighbouring thunk builds with `lui a0,0x18; addiu a0,a0,0x2240` and passes as a callback. It sat inside `ptr_00182230` (the gap pass ran it up to the next known start) | `add_pointer_targets` now also takes addresses built with lui+addiu/ori in code, and accepts targets inside a known range when they follow an unconditional `j`/`jr` + delay slot: +63 functions (257 total). Overlay discovery uses the same two rules. Verified with ps2_recomp: `ptr_00182240` -> `FUN_0016ed80` registered |
| (none) | After `MCMENU\MCMENU.DAT` loads the game goes silent: no more per-frame sound polls, no error, `Game thread returned. PC=0x0` at close (scheduler idle = every guest thread blocked). Suspected: libmc2 (`sceMc2Sync`, completion via `sceMc2SifCallback`) waiting on an IOP->EE notification | Watchdog result: not a deadlock. T1 (main thread, entry 0x100008) is DORMANT with pc=0 right after MCMENU.DAT loads; T2 (0x2DFD00) and T3 (0x2E98F0) are idle workers on their semaphores; EE cycles keep advancing. The memory-card scene's init thunk (scene table 0x3A7FF0, entry `0x182250` -> callback 0x182240 -> `FUN_0016ed80` scene switcher, update `FUN_002a2160`) is the first new code to run. Trace: `main` never returns and `exit` is never called; the scene switch creates task `FUN_002a2160`, which calls MCMENU 0x738DE0 (reads MCMENU.DAT, the thread blocks and unwinds as designed) and the thread is gone right after the read completes and the GS interrupt handler (`FUN_002cbb40`, installed with AddIntcHandler(0)) runs. All return points are registered (main + MCMENU). Patch 0009 makes the scheduler log why a thread stops (pc 0 / ExitThread) with its last 24 dispatched PCs |
| 0x2A1F84 | Patch 0009 report: `thread 1 stopped: returned to pc 0`, last dispatches `0x2a1f44 -> 0x2a1f3c -> 0x2a1f84(ra=0x0)`. Task `0x2A1F20` saves ra at sp+0x10, calls MCMENU 0x739260, and its `ld ra, 0x10(sp)` reads 0: sp was 0xE0 too low. Overlay discovery had started a function at 0x7395F0, which is the delay slot of 0x739260's `jr ra` (`addiu sp, sp, 0xE0`). Split there, the recompiler returned without the delay slot, so the stack frame was never popped (the interrupt-stack overlap suspected first was ruled out: callback stacks sit at 0x52000-0x80000) | The "after j/jr + delay slot" rule tested the jump at v-4 (making v the delay slot itself). Fixed in tools/rd_elf.py (`has_delay_slot`, `in_delay_slot`, `after_unconditional`, shared by rd_mkconfig and rd_overlay): no start may sit in a delay slot (REGIMM mtsab/mtsah are not branches). Drops 15 bad overlay starts (MCMENU, AREA01/04, IMENU, M1_*, ...) and 5 main-ELF pointer targets, two of them the same `jr ra; addiu sp` split (0x20B590, 0x2A0000). `clean_function_map` also merges the two Ghidra `entry_` functions that start in a branch delay slot (0x2B5E44, 0x2C62B0) back into the function holding the branch; ps2_recomp then emits the branch-into-delay-slot label itself (checked). MCMENU now registers 207 entry points. Needs `rd config` + `rd recomp` + `rd overlays` + `rd build` |
| (input) | First in-game screen: the memory-card dialog (frame + YES/NO, cursor on NO) renders and T1 runs its frame loop, but nothing reacts to keys. The game reads the pad through libpad2 (`scePad2*`), fed by DBCMAN/DS2U on the IOP over DMA; ps2xIOP's DBCMAN HLE only answers the version query, so no pad data ever arrives | rd_game.cpp `setupPadHle` (`[hle] pad = 1`) answers the six calls the game's pad routine `FUN_002dc110` uses: `scePad2CreateSocket` (socket per work area), `scePad2GetState` (1 = stable on socket 0), `scePad2GetButtonProfile`, `scePad2Read`, `scePad2GetButtonInfo` and `sceVibGetProfile` (0, no rumble), plus a no-op `sceVibSetActParam`, from the runtime's host pad backend. Index map from the game's mask table 0x43F170: 0-15 = raw DualShock2 button bits (SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1 TRI CIRCLE CROSS SQUARE), 16-19 = LX LY RX RY (FUN_002da9a0 turns them into direction bits), 20-31 pressure. Keys: arrows/WASD, X/Space = cross, C/Esc = circle, Z = square, V = triangle, Q/E = L1/R1, Shift = L2/R2, Enter = start, Tab = select; a gamepad works too |
| 0x212DC8 | Input works; memory-card dialog answered, the publisher logos show (distorted), then the game freezes: `ROOT.DAT` streaming starts (`sceCdStStart`/`sceCdStRead`), the watchdog snapshot stops changing, and at close T1 is at 0x212DC8 (ra 0x2DFAB8). That is the FMV player `FUN_00212bc0` waiting for the end of the movie (`while (!FUN_0020da80()) FUN_0020f340();`). Movies are PSS streams decoded by libmpeg on the IPU, and ps2recomp has no IPU/MPEG-2 decoder, so no frame ever completes | `setupMovieHle` (`[movies] skip = 1`) replaces the player with an immediate return; its only caller, the play-movie wrapper `FUN_00212f00`, still sets up and restores the GS mode. Real playback (host MPEG-2 decoder for the PSS video, ADPCM/PCM audio) is a ROADMAP item |
| 0x29E150 | With movies skipped, `TITLE.BIN` loads (1576/1576 entry points) and immediately: `guest-branch:missing-target` on a `jal 0x29E150` from TITLE (0x7477D0). 0x29E150 is a 3-instruction getter (`lui v0,0x69; jr ra; lw v0,0x54D4(v0)`) right after another function's `jr ra`, called only from overlays, so neither Ghidra nor the main-ELF pointer pass knew it. Across all 79 overlays there are 558 such main-ELF call targets with no function | `rd config` now reads the extracted overlays (`--overlays work/overlays`, `overlay_main_refs` in tools/rd_overlay.py): every jal/j target an overlay has in the main ELF becomes a function (even inside a Ghidra range, never in a delay slot), and overlay data words are checked like the ELF's own data pointers. Pointer/overlay functions: 252 -> 810; all 1812 overlay call targets now have an entry. Checked with ps2_recomp (`ptr_0029e150` generated). Needs `rd config` + `rd recomp` + `rd build` (overlays must be extracted first) |
| (graphics) | **Main menu reached** (title map + menu, no errors, all threads healthy). Everything is drawn distorted: the publisher logo sheared/split, the title map posterised and blocky with stray lines, message text missing in the memory-card dialog. That is the runtime's GS emulation (CPU rasterizer + texture/CLUT/swizzle handling + display-buffer selection), not the game code | Renderer work starts from GS captures: patch 0010 + F9 (above). Next fixes come from replaying the user's captures here |
| (graphics) | First two captures replayed offline: both match the user's screen exactly, so the CPU rasterizer reproduces what the game sends. Texture uploads and swizzling are correct (the title map decodes cleanly from VRAM as PSMT8 indices). The breakage is in the **palettes (CLUTs)**: (1) memory-card dialog: all 264 message-text glyphs are drawn (font PSMT4 512x512 at 0x34B0, alpha test GREATER 0) but their CLUT at block 0x36B0 is all zeros, so every texel has alpha 0 and fails the test - the text is invisible; the neighbouring CLUTs (0x36D1, 0x36F2, 0x3AF3) of the YES/NO/frame textures are fine. (2) title map: TEX0 = 0x20069606A93434B0 has CBP = TBP = 0x34B0, so the 256-colour palette is read from the image's own pixels -> posterised blue/white. In the previous frame a palette was uploaded to 0x3CB0 (= TBP + 0x800) but no draw used it | Both CLUT uploads/addresses were set before the captures started. Next: a capture from boot (`--gscap 1 --gscap-frames 100000`, F9 to stop) to see every texture + CLUT upload and the TEX0 the game builds for them |
| (graphics) | Boot capture (290 frames: memory-card dialog, Irem, Agetec, title) found the cause: the game uploads textures and CLUTs over **VIF1 DMA chains with TTE**: `DIRECTHL n` carrying the GIF A+D setup and an IMAGE tag, then the next DMA tag's VIFcodes `NOP; DIRECTHL m` and the pixel data. ps2recomp treated whatever followed a DIRECT that ended in an IMAGE tag as raw image bytes, so the two TTE VIFcodes became the first 8 bytes of every image/CLUT and the last 8 bytes were then decoded as VIFcodes. Result: every palette shifted by two entries with `0x51000040` in it, CLUT uploads after it lost, sheared textures. Visible as missing text, posterised title map, wrong logo colours | Patch **0011** (`ps2_vif1_interpreter.cpp`): an IMAGE tag still owed data continues in the next DIRECT/DIRECTHL payload (synthetic IMAGE tag for the GS frontend), and the VIF keeps decoding VIFcodes in between; the old raw continuation is kept only when the next VIFcodes are not NOPs leading to a DIRECT (ps2recomp's own tests for it still pass). New test `VIF1 DIRECT image tag continues in the next DIRECTHL`. ps2recomp test suite built and run here: 446/447 pass, the one failure (`VU0 macro mappings cover all S1/S2 enums`) fails the same without the patch. Captures repaired the same way render the Irem logo in its real colours |
| (graphics) | With 0011: memory-card text, Irem/Agetec logos and the title's copyright text and logo render; the title map is covered in 1-pixel stripes, checkerboards and line patterns. Traced with the boot capture (`whodraw`: which record changes a pixel): the full-screen 2D quads (two triangles, Z = 0xFFFFFF00, XYOFFSET 1728/1824, ZTST GEQUAL against a Z buffer holding 0xFFFFFF00) failed the depth test on scattered pixels. The CPU rasterizer interpolates Z with float barycentric weights; near 0xFFFFFFFF an error of 1e-7 in the weights moves Z by hundreds | Patch **0012** (`gs_cpu_backend.cpp`): constant-Z triangles/lines keep their exact Z, interpolated Z is clamped to the vertices' range. New test with the game's numbers (fails without the fix, passes with it); suite 447/448 (same pre-existing VU0 failure). Replaying the capture now gives a clean title screen with START GAME / LOAD GAME / TUTORIAL / OPTION |
| (speed) | Title, difficulty menu and NEW GAME work; after choosing the difficulty CHARSEL.DAT loads 6 character models (IMD) and the screen stops changing (next screen: Joshua's 3D model). The frame loop keeps running (GsGetIMR/GsPutIMR every frame, EE cycles advancing) at about 2 frames/s. Profiling the GS replay: one title frame is ~4.6 M textured pixels, ~18 M bilinear texel reads, all on one thread in the CPU rasterizer (~0.5 s/frame on the 2-core sandbox) | Patch **0013**: big sprites/triangles are split into row bands drawn on a worker pool (`PS2_GS_THREADS`, default = hardware threads; primitives stay in order), each thread with its own texture page cache (generation-checked so TEXFLUSH/reset still drop it). Replays are byte-identical serial vs parallel; 1.56x on 2 cores here, more on a desktop CPU. Suite 447/448. The real fix for speed is the GPU renderer (ROADMAP Phase 4) |
| (graphics) | Character select (Joshua): the game is not hung, but the screen keeps showing the title. (First read of the capture: "all 3D draws land on one spot" - wrong, only the first few untextured strips are tiny; see the next row.) The models go through **VU1** (VIF1 UNPACK of vertex data + VU1 microprogram + XGKICK), not through GIF packets the CPU builds, so the GS capture only shows the broken result | Patch **0014**: a GS capture also records every VIF1 stream (record 9) and, at its start, VU1 code/data memory, the VIF1 registers and the VU1 registers (record 10). `tools/vu_replay` (built against the runtime library, see its header) replays those streams through the runtime's own VIF1 + VU1 interpreters and prints what VU1 sends to the GS, so the wrong stage (UNPACK, microprogram, XGKICK) can be found offline. gs_replay skips the new records. Suite 448/448 when `ps2x_tests` is run from `ps2xTest/` (the VU0 macro test reads a file relative to the cwd; the earlier "1 failure" was only that) |
| (graphics) | Capture with 0014 replayed through `vu_replay`: VU1 output is identical to what the game sent and draws a correct Joshua model. Every draw of the scene (background, panels, model) failed the **depth test**: ZTST GEQUAL against a Z buffer still holding 0xFFFFFF00 from the title map sprite, and nothing ever cleared it. The game clears through libgraph: `sceGsSetDefDBuff(db, psm, w, h, ztest, zpsm, clear)` (0x2CC208, ztest/zpsm/clear in t0-t2) + `sceGsSwapDBuff` (GIFtag NLOOP 14 = draw env + clear sprite), and a scene clear packet built with `sceGsSetDefClear` at 0x2CB95C and sent by VIF1 DIRECTHL. In the runtime `sceGsSetDefClear` was an empty stub, `sceGsSetDefDBuff` read those three args from the stack (garbage) and never seeded the clear, and `sceGsSwapDBuff` only sent the 8 draw-env registers | Patch **0015** (`Kernel/Stubs/GS.cpp`): `sceGsSetDefClear` writes the same 6 A+D pairs as the game's libgraph (disassembled at 0x2A5940); `sceGsSetDefDBuff` takes ztest/zpsm/clear from t0-t2 (stack as fallback, like the Dc variant), sets NLOOP 8/14 and seeds both clears; `sceGsSwapDBuff` sends NLOOP pairs (env + clear); `sceGsPutDrawEnv` sends the tag's NLOOP pairs when a0 points at an A+D GIFtag (libgraph's real signature). Two new tests; suite 450/450. Replaying the capture with Z cleared gives the full character select screen (background, photo, PLAYER panel, Joshua) |
| 0x200310 | In game (Shinohara_Opening script) the main thread died: `guest-branch:missing-target kind=DirectJump op=J source=0x1CE0D8 target=0x200310`. 0x200310 is a 5-instruction leaf (no prologue) reached only by a tail call `j`; Ghidra merged it into the function before it | `rd_mkconfig.py` now also takes the main ELF's own j/jal targets and adds those that sit in a gap or right after jump + delay slot (same rule as pointer targets): 55 new entries, all real leaf functions or prologues, including 0x200310. Needs `rd.py config` + `rd.py recomp` + `rd.py build` |
| (graphics) | In game: grey/white "exploding" polygons, no textures. Capture + `vu_replay`: VIF and VU1 are fine; the object (world) matrix the EE uploads to VU1 qw 4-7 already has **four identical rows** (0.45,0,0,0) in RAM, so VU1 program 0 (matrix setup) builds garbage and every vertex smears across the screen. The EE builds matrices with libvu0 (`sceVu0MulMatrix`, `RotMatrix`, `CameraMatrix`, ...), which ps2recomp replaces by C++ approximations in `Kernel/Stubs/VU.cpp` ("reconstructed from behavior"). Checked against the game's own code: `sceVu0MulMatrix(m0,m1,m2)` is m0 = m2·m1 (loads m1 rows, combines them per row of m2); the stub computes m1·m2 | `rd_mkconfig.py` no longer passes the `sceVu0*` stubs (28) to the recompiler, so the game's own libvu0 code is recompiled (the recompiler covers every VU0 macro instruction they use; checked with ps2_recomp on the real config: no unhandled instruction in 0x2A6C08-0x2A7900). List: `RECOMPILE_INSTEAD_OF_STUB`. Needs `rd.py config` + `rd.py recomp` + `rd.py build` |
| (graphics) | Still exploding after the 28 `sceVu0*` were recompiled. Capture: VU1 program 0 builds MVP = VP x M from qw 4-7 = the object matrix (copied by the EE from obj+0x40 at 0x3110F8), and program 0x2F0 transforms every vertex with it; M has four identical rows (s,0,0,0). Cause: libvu0's internal helper `_sceVu0ecossin` (0x2A6F58, angle in and cos/sin out through VU0 registers, used by every RotMatrix*) was still bound - the underscore name escaped the `sceVu0` filter and the recompiler strips the underscore and calls the runtime's C-ABI `sceVu0ecossin(dst=a0, angle=f12)` | The filter compares `name.lstrip('_')`: 29 functions recompiled, `_sceVu0ecossin` included (checked with ps2_recomp on the real config; no stub call or unhandled instruction left in 0x2A6C08-0x2A78F0 except sceVpu0Reset). Needs `rd.py config` + `rd.py recomp` + `rd.py build` |
| (graphics) | Still exploding with all 29 libvu0 functions recompiled. Host harness (`/tmp/vh`, links the generated FUN_*.cpp with the runtime library): libvu0 Unit/RotX/Y/Z/RotMatrix/MulMatrix and the game's own object-matrix helpers (0x310A50 unit, 0x310A20 length via vsqrt, 0x3109A0 4x4 multiply, 0x310990 lq/sq copy, 0x310960 store to obj+0x40) all give exact results; no recompiled instruction writes vf0. So the code is right and the inputs reaching it are not | Open: `rd.py run --trace 0x310960!m,0x3109A0!m,0x310A80!m,0x310710!m` to see the matrices flowing into the object matrix in game |
| (graphics) | `!m` trace in game: every EE matrix step is right (parent x scale, translation, stored to obj+0x40), the matrices live in **scratchpad** (0x70000000), yet VU1 got the same (s,0,0,0) rows for every object and frame. The runtime's DMA chain walker read DMAtag ADDR as `(tag >> 32) & 0x7FFFFFFF`, dropping bit 31 = **SPR** (data/next tag in scratchpad), so a REF to scratchpad offset X read RAM at X. MADR/TADR/ASR with bit 31 had the same problem, and the `sceDmaSend` stub turned a scratchpad pointer 0x7000xxxx into 0x1000xxxx (I/O) | Patch **0016** (`ps2_memory.cpp`, `Stubs/Helpers/Support.h`): `dmacAddress()` maps bit-31 addresses to the scratchpad window (tags, MADR, TADR, ASR0/1, native GIF chains), TADR/ASR are written back in DMAC form, `toDmaPhys` keeps scratchpad pointers. New test (REF tag with the SPR bit takes its data from scratchpad; fails without the fix). Suite 451/451. Needs `rd.py build` |
| (graphics) | Still exploding with 0016. Same-run `!o` trace: the object path (0x3106xx -> 0x310960) stores correct matrices into the scratchpad object (0x700006A0+0x40) and the packets carry them. Per-class replay of the capture (`VUR_CLASSES=1 vu_replay`): of ~80k VU1 vertices, 62.6k come from batches whose qw 4-7 is (s,0,0,0)x4 - the **map** batches - while objects with real matrices are a minority. Map path: FUN_002344A0 copies a node matrix from the level data, runs 0x2D9C00(m, m, obj+0x380) (normalize rows + atan2f, all recompiled game code - correct on clean inputs in the host harness), stores it at obj+0x40 and in the packet | Open: `rd.py run --trace 0x2D9C00!m,0x2344A0!o` to see the map node matrix and parent going in |
| (graphics) | `!o` trace on the command handlers: **0x310D80** turns a correct object matrix into `(0.4565 0 0 3.4e10)` x4, then 0x311960 packs it for VU1. 0x310D80 loads the map piece matrix from a palette in **VU0 data memory**: `ctc2 v0,$vi1` + 4x `vlqi.xyzw vfN,(vi1++)`; the palette is filled earlier with `vsqi` (0x30DA44..). The recompiler translated VLQI/VSQI/VLQD/VSQD as `READ128/WRITE128((vi & 0x3FF) << 4)` = **EE RAM 0x0000-0x3FF0**, not VU0 data memory, so the palette went into (and came back out of) low RAM and got clobbered; VILWR/VISWR used `vi << 2` the same way | Patch **0017** (`ps2xRecomp/src/lib/vu_translation_helpers.cpp`): VLQI/VSQI/VLQD/VSQD/VILWR/VISWR address `0x11004000 + (vi & 0xFF) * 16` (+ field offset for VILWR/VISWR), vi stays 16-bit, vi0 never modified. New code-generator test; suite 452/452. `rd.py recomp` now applies patches and rebuilds ps2_recomp itself (recompiler patches need a re-recompile, not just a build) |
| (graphics) | Screen recording with 0017 (debug UI build): boot, logos, memory-card dialog, title menu and character select draw right; in the opening the exploding shards are gone, replaced by large flat grey untextured polygons over a white clear colour. `TexLoadImagePacket2: TEXimage == NULL` x40 in run.log. The F1 debugger log dump only holds GsGetIMR/GsPutIMR polling | Paused: evaluating ps2-native-recomp (docs/NATIVE_RECOMP.md), which emulates IOP/SPU2/IPU and draws the GS on the GPU. If ps2recomp work resumes: missing textures first (TexGetTEXimage 0x2D6A50) |
| (graphics) | After the libvu0 fix the geometry is in place (flat grey walls/floor in perspective) but **untextured**: the game prints `TexLoadImagePacket2: TEXimage == NULL` (rate-limited at 40), i.e. `TexGetTEXimage(id)` (0x2D6A50: id -> table at 0x6AA9A0) finds no registered texture, so no TEX0 is sent (draws with TBP 0, TW 0). Also `T2ImdCheckFileLOD: file not exist` (0x2CA040 file check -> 0x2C8C20 FAT search, which compares names with the game's strcasecmp at 0x2E5398) and `EffObjInit: Can't Find IMD` | Open: traced with `rd.py run --trace 0x2D6A50!z,0x2C8C20!z,0x2CA040!z` to see which texture ids and file names fail and who asks |

Next binds the game will make (checked in a ps2xIOP test with the real IRX):
Irem's sound server 0x12346 registers once IREMSND2's thread runs, so
`FUN_0030a000` should bind; DBCMAN 0x80001300 and LIBSD 0x80000701 are served
by ps2xIOP's HLE services.

Guest memory layout (`[memory]` in rawdanger.ini):

| range | owner |
|---|---|
| 0x00020000-0x00051000 | ps2recomp runtime pools (RPC packets/servers, TLS, bootmode), moved here by patch 0003 |
| 0x00052000-0x00080000 | runtime interrupt/callback stacks (patch 0003) |
| 0x00100000-0x0048F300 | game ELF (code + data, one W+X segment), then BSS and MWo3 overlays |
| up to 0x01FE8000 | game heap (`MemutilInit` takes ~24.4 MB). Upstream caps the heap at 0x01F00000 and `SetupHeap` reset it; patches 0003 + 0004 make both honour this limit |
| 0x01FE8000-0x02000000 | main thread stack (`SetupThread(stack=0x01FE8000, size=0x18000)`) |

`layout = upstream` restores ps2recomp's defaults (the game then fails with
`MemutilAllocAlign: Alloc Error`).

