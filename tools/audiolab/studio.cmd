@echo off
chcp 65001 >nul
rem The sound studio: the page for recording and listening by hand.
rem
rem The interpreter is looked for exactly as audiolab.cmd looks for it - an
rem environment made in this folder (requirements.txt), otherwise `python` on
rem the path, and AUDIOLAB_PYTHON over both. It used to borrow the environment
rem of the voice service, and that service is gone.
cd /d "%~dp0"
set "PY=python"
if exist ".venv\Scripts\python.exe" set "PY=.venv\Scripts\python.exe"
if not "%AUDIOLAB_PYTHON%"=="" set "PY=%AUDIOLAB_PYTHON%"
"%PY%" server.py %*
set CODE=%ERRORLEVEL%
echo.
echo The studio has stopped, code %CODE%. The log: studio.log
rem The window does not close by itself. The studio once vanished together with
rem it, and there was nowhere left to read the reason: it went to the screen,
rem and the screen was already gone.
pause
