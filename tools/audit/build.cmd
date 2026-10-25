@echo off
rem Builds NativeTweaksAudit.exe (offline Vanilla Settings pattern check).
setlocal
if not defined NT_VCVARS set "NT_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%NT_VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
set SRC=..\..\src
cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"..\..\third_party\ERNativeUI\include" ^
  /Fo:out\ /Fe:out\NativeTweaksAudit.exe audit_main.cpp %SRC%\vanilla_settings.cpp %SRC%\game_memory.cpp ^
  %SRC%\live_text.cpp %SRC%\traditional_chinese.cpp %SRC%\hotkeys.cpp %SRC%\perf_log.cpp spirit_ashes_stub.cpp ^
  user32.lib version.lib || exit /b 1
echo Built out\NativeTweaksAudit.exe
