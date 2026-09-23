@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"

rem build the fixed DLL under a different name (tf2perf.dll is locked while TF2 runs)
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /I "C:\mh\include" tf2perf.c /LD /Fe:tf2perf_fixed.dll /link "C:\mh\lib\libMinHook.x64.lib"
if errorlevel 1 goto fail

cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS tf2perf_cli.c /Fe:tf2perf.exe
if errorlevel 1 goto fail

echo.
echo build ok: tf2perf_fixed.dll + tf2perf.exe
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
