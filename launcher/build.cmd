@echo off
rem Rebuilds ..\RawDangerPC.exe from the .cs files with the C# compiler that comes with Windows 10/11 (.NET Framework 4).
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" ( echo csc.exe of .NET Framework 4 not found & pause & exit /b 1 )
"%CSC%" /nologo /optimize+ /target:winexe /win32icon:"%~dp0art\app.ico" /out:"%~dp0..\RawDangerPC.exe" /r:System.Windows.Forms.dll /r:System.Drawing.dll /resource:"%~dp0art\app.ico",RawDangerLauncher.app.ico /resource:"%~dp0art\app.ico",RawDangerLauncher.app.ico /resource:"%~dp0art\logo.png",RawDangerLauncher.logo.png /resource:"%~dp0art\logo_jp.png",RawDangerLauncher.logo_jp.png /resource:"%~dp0art\characters.txt",RawDangerLauncher.characters.txt "%~dp0Launcher.cs" "%~dp0WizardForm.cs" "%~dp0Disc.cs" "%~dp0Cast.cs" "%~dp0Icons.cs" "%~dp0Enhanced.cs" "%~dp0Recomp.cs" "%~dp0Music.cs" "%~dp0Achievements.cs" "%~dp0Flac.cs" "%~dp0BuildTime.cs" "%~dp0EngineUpdates.cs" "%~dp0BuildTools.cs"
if errorlevel 1 ( pause & exit /b 1 )
echo built: %~dp0..\RawDangerPC.exe
