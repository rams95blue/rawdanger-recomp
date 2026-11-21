#!/bin/sh
# Rebuilds ../RawDangerPC.exe with Mono's mcs (Linux). Compiled against the .NET Framework 4.5 reference assemblies, so it only
# calls what Windows' .NET Framework 4 has: Mono's own mscorlib has newer overloads (String.Split(char, options), ...) that
# Windows does not, and the launcher would stop at start with "method not found".
set -e
cd "$(dirname "$0")"
A=/usr/lib/mono/4.5-api
mcs -nologo -optimize+ -target:winexe -nostdlib -lib:$A -r:$A/mscorlib.dll -r:$A/System.dll -r:$A/System.Core.dll \
    -r:$A/System.Drawing.dll -r:$A/System.Windows.Forms.dll -win32icon:art/app.ico -out:../RawDangerPC.exe \
    -resource:art/app.ico,RawDangerLauncher.app.ico -resource:art/logo.png,RawDangerLauncher.logo.png -resource:art/logo_jp.png,RawDangerLauncher.logo_jp.png \
    -resource:art/characters.txt,RawDangerLauncher.characters.txt \
    Launcher.cs WizardForm.cs Disc.cs Cast.cs Icons.cs Enhanced.cs Recomp.cs Music.cs Achievements.cs Flac.cs BuildTime.cs EngineUpdates.cs
echo "built: ../RawDangerPC.exe"
