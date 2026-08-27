@echo off
set "TDESKTOP_WORKDIR=%APPDATA%\Telegram Desktop"
start "" "%~dp0Telegram.exe" -workdir "%TDESKTOP_WORKDIR%"
