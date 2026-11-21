@echo off
rem Raw Danger! on ps2-native-recomp: replay the newest F9 report headless for ~10 s of game time
rem with disc, stream, SIF and syscall tracing, plus a picture every second.
rem Put this file in the ps2-native-recomp-main folder and double-click it.
setlocal
cd /d "%~dp0"
set "G=games\Raw-Danger"
set "R="
for /f "delims=" %%d in ('dir /b /ad /o-d "%G%\reports" 2^>nul') do if not defined R set "R=%G%\reports\%%d"
if not defined R (
  echo No report found in %G%\reports - press F9 in the game first.
  pause
  exit /b 1
)
set "OUT=%G%\diag"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
copy /y "%R%\report.state.ps2" "%OUT%\card.ps2" >nul
echo report: %R%> "%OUT%\which.txt"

set PS2_TRACE_CD=1
set PS2_TTY_FRAME=1

for /f "tokens=3 delims=_" %%f in ("%R%") do set "F=%%f"
rem F is like f009908 (6 digits): prefix a 1 so it is read as decimal, not octal
set /a FIRST=1%F:~1% - 1000000
set /a LAST=FIRST+600
echo replaying from frame %FIRST% to %LAST% ...

"build\native-Raw-Danger\ps2run.exe" "%G%\patched\SLUS_215.01" --disc "%G%\disc.iso" --game-dir "%G%" ^
  --widescreen --patches "%G%\patches.txt" --memcard "%OUT%\card.ps2" --no-record ^
  --load-state "%R%\report.state" --frames %LAST% --frame-every 60 --frames-dir "%OUT%" ^
  --trace-syscalls --trace-threads --trace-iop-hle > "%OUT%\diag.log" 2>&1

echo.
echo done: %OUT%\diag.log  (and the pictures next to it)
pause
