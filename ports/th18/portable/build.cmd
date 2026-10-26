@echo off
rem TH18 reconstructed build (Win32/x86, MSVC + Ninja).
setlocal
set ROOT=%~dp0..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cmake -S "%ROOT%" -B "%ROOT%\build\native" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build "%ROOT%\build\native" %*
