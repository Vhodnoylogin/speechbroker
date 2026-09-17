@echo off
chcp 65001 >nul
rem The commands of the tooling: record, listen, calibrate, bench, corpus,
rem replay, scenario, prosody. Without an argument it prints the list.
rem
rem The interpreter is looked for in one order and written down nowhere else:
rem an environment made in this folder (requirements.txt says what goes into
rem it), otherwise whatever `python` is on the path. AUDIOLAB_PYTHON overrides
rem both - a machine where the environment lies elsewhere sets it, and nothing
rem in the repository has to know where that is.
cd /d "%~dp0"
set "PY=python"
if exist ".venv\Scripts\python.exe" set "PY=.venv\Scripts\python.exe"
if not "%AUDIOLAB_PYTHON%"=="" set "PY=%AUDIOLAB_PYTHON%"
"%PY%" -m audiolab %*
set CODE=%ERRORLEVEL%
rem Started by a double click there is no argument, no terminal to read and the
rem window would close on the list of commands. Started from a terminal there
rem is always an argument, and waiting for a key would be in the way.
if "%~1"=="" pause
exit /b %CODE%
