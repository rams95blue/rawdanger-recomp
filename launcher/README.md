# Raw Danger PC (v0.5.0)

A wizard that turns your Raw Danger! / Zettai Zetsumei Toshi 2 disc images into native PC games. The
ps2-native-recomp engine is inside this folder (`engine\ps2-native-recomp`, MIT licence), so nothing else is needed.

## Use

1. Unzip `rawdanger-recomp` next to your disc folders:
   ```
   RAW DANGER DECOMP\
     GAME US\Raw Danger (USA).iso
     GAME JP\... (Japan).iso
     GAME JP\DEMO\... (Taikenban Type-A).cue/.bin, ... (Type-B).cue/.bin
     rawdanger-recomp\RawDangerPC.exe        <- start this
     rawdanger-recomp\engine\ps2-native-recomp\
     rawdanger-recomp\patches\pnach\
   ```
2. **Which disc?** Every .iso / .cue (and .bin without .cue) found is listed with its serial; tick one or more, or
   **Browse for a disc image...** anywhere else (remembered in `launcher.ini`).
3. **Options**: widescreen (on), post-processing filter removal (off, not needed natively), desktop shortcuts (on).
   The fast loading fix is always applied: without it the game waits for ever on its first loading screen.
4. **Build**: progress per step, details on request, Stop. The first build gets the build tools once: copied from a
   `ps2-native-recomp-main` folder next to this one if you have one set up, else downloaded (~312 MB). 5-15 min per disc.
5. **Done** / **Your games**: Play, Desktop shortcut (`RawDangerPC.exe --play <name>`), Make it faster (rebuilds with
   the code recorded while you played), Build again, Open its folder.

If anything goes wrong, `launcher.log` (next to the exe) and `engine\ps2-native-recomp\games\<name>\build.log` say why.

## How it builds

The ps2-native-recomp app's own steps, as one PowerShell script per build (`games\<name>\launcher_build.ps1`):
`scripts\portable.ps1 -ForApp` if the tools are missing, `tools\portable\env.ps1`, `scripts\setup.sh --native-only` if
the translator is missing, then `scripts\port.sh <name> <image> --native-only --pnach <file> --pnach-section ...`;
afterwards `games\<name>\app.json` in the app's format. Folders: `RawDanger-US`, `ZZT2-JP`, `ZZT2-DemoA`, `ZZT2-DemoB`.

## Build the exe yourself

`launcher\build.cmd` compiles `Launcher.cs`, `WizardForm.cs` and `Disc.cs` with the csc.exe of .NET Framework 4 that
Windows 10/11 already has (C# 5, no extra DLLs) into `RawDangerPC.exe`.

The title art (`launcher\art\logo.png`) is built into the exe; a `logo.png` next to `RawDangerPC.exe` replaces it.

## Language

The **日本語 / English** button in the header switches the whole launcher (it reopens in the other language); the choice
is kept in `launcher.ini` (`Language=jp` or `en`). With no choice saved it follows the Windows display language.

## The cast

The characters cycle on the right of the window on every page, including while a game builds: a picture, the US and
Japanese names, chapter, age, job and a short background. Hover to hold one, click for the next (right-click: back).
The text is `launcher\art\characters.txt` (built into the exe; a `characters.txt` next to the pictures replaces it).
The pictures are not shipped with the launcher: it reads them where they are on your PC, from a folder named `charlaunch` (or `MODPHOTOS`) near it, from the folder in
`CastPictures=` in `launcher.ini`, from `art\characters\` next to the exe, or from a saved character-guide page
(`*_files` folder with `f_shinohara.jpg` ...) within two folders of it. A character without a picture shows a plain sign.

## Enhanced Edition (the fifth game)

Offered once all five discs are found (US, EU, JP, JP demo Type-A, JP demo Type-B). It is built from the US disc into its own
folder (`games\RawDanger-Enhanced`) with the fast-loading fix, widescreen 16:9 and 2x internal resolution as the default
(F1 in the game changes it). The roadmap's next features, taken from the JP discs (Japanese voices, JP-region textures),
will be added to this edition.

## Icons and names

`RawDangerPC.exe` uses the launcher's warning sign as its icon. Each desktop shortcut gets its own sign with the version
written on it (US, JP, JP TYPE A, JP TYPE B, EX), saved as `games\<name>\<name>.ico`. The game window's title ends in
"rawdanger-recomp", and so do the launcher's title bar and the exe's file details.

## Offline build tools (tools pack)

`rawdanger-recomp-tools.zip` holds the build tools' original archives. Unzip it over `rawdanger-recomp` so they land in
`engine\ps2-native-recomp\downloads\`; the build scripts use them (after checking their SHA-256) instead of downloading.
`downloads\TOOLS.txt` lists each file, its source and licence, and the two small files not in the pack (Python, BusyBox).

## Recomp options (launcher and in game)

Each build writes `games\<name>\recomp.ini`, one entry per patch. *Recomp options* in "Your games" and the game's F1
menu (*Recomp options*, or F3 in the game) read and write the same file. Live options (data-only patches such as
widescreen, marked `// recomp: live` in the .pnach) switch at once, even while playing; built-in ones (code patches)
change by building again.

## After a build

Temporary files are removed: `generated\<name>`, the compiler's objects in `build\native-<name>` (the game program and
its DLLs stay), the test-run state, and the copy of an .iso disc image (the game then reads your original image).
The tools pack (`downloads`) is never touched. Never delete `games\<name>`: it holds the saves, settings and caches.
