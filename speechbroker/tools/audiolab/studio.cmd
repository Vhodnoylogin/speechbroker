@echo off
chcp 65001 >nul
rem Студия звука. Своей виртуальной среды у модуля нет - берём среду
rem голосовой службы, где sounddevice, numpy и piper уже стоят.
cd /d "%~dp0"
"..\voice\.venv\Scripts\python.exe" server.py %*
set CODE=%ERRORLEVEL%
echo.
echo Студия остановлена, код %CODE%. Журнал: studio.log
rem Окно не закрывается само. Однажды студия исчезла вместе с ним, и причину
rem прочитать было негде: она ушла на экран, а экрана уже не было.
pause
