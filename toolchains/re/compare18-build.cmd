@echo off
rem Build the TH18 comparison launcher (Win32/x86).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /std:c++20 /EHsc /W3 compare18.cpp /Fe:compare18.exe /Fo:compare18.obj
exit /b %errorlevel%
