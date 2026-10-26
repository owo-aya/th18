@echo off
setlocal
set "ROOT=%~dp0.."
start "" "%ROOT%\build\native\th18.exe" --game-dir "%ROOT%\..\..\toolchains\game18" --user-dir "%ROOT%\build\native"
