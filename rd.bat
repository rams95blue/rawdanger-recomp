@echo off
rem Windows wrapper: rd setup / extract / compare / analyze / config / recomp / build / voicepack / run / all
setlocal
where py >NUL 2>NUL
if %errorlevel%==0 (
  py -3 "%~dp0rd.py" %*
) else (
  python "%~dp0rd.py" %*
)
exit /b %errorlevel%
