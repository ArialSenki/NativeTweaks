@echo off
rem Builds and runs the offline NativeTweaks tests (no game needed).
setlocal
if not defined NT_VCVARS set "NT_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%NT_VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo:out\ /Fe:out\hotkeys_test.exe hotkeys_test.cpp ..\src\hotkeys.cpp ..\src\perf_log.cpp user32.lib kernel32.lib || exit /b 1
out\hotkeys_test.exe
