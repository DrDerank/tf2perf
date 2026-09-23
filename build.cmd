@echo off
setlocal
rem MinHook location: set MINHOOK env var, defaults to C:\mh
if "%MINHOOK%"=="" set MINHOOK=C:\mh
rem vcvars64.bat location: set VCVARS env var if yours differs
if "%VCVARS%"=="" set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat

if not exist "%MINHOOK%\include\MinHook.h" (
  echo [-] MinHook not found at %MINHOOK% - set MINHOOK to your MinHook folder
  exit /b 1
)
if not exist "%VCVARS%" (
  echo [-] vcvars64.bat not found - set VCVARS to your Visual Studio path
  exit /b 1
)

call "%VCVARS%" >nul
cd /d "%~dp0"

echo === building tf2perf.dll ===
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /I "%MINHOOK%\include" tf2perf.c /LD /Fe:tf2perf.dll /link "%MINHOOK%\lib\libMinHook.x64.lib"
if errorlevel 1 goto fail

echo === building tf2perf.exe ===
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS tf2perf_cli.c /Fe:tf2perf.exe
if errorlevel 1 goto fail

echo === building sigcheck.exe ===
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS sigcheck.c /Fe:sigcheck.exe
if errorlevel 1 goto fail

echo.
echo build ok: tf2perf.dll + tf2perf.exe + sigcheck.exe
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
