@echo off
setlocal

rem RidingPlugin standalone (launcher + bootstrap)
rem Toolset: VS2010 v100 x64

set "SRCDIR=D:\KenshiModDev\RidingPlugin\standalone"
set "OUTDIR=D:\KenshiModDev\Build\RidingStandalone"
set "VC=C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64"
set "SDKINC=C:\Program Files\Microsoft SDKs\Windows\v7.1\Include"
set "SDKLIB=C:\Program Files\Microsoft SDKs\Windows\v7.1\Lib\x64"
set "VCINC=C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\include"
set "VCLIB=C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\lib\amd64"

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem ---- Bootstrap DLL ----
call "%VC%\cl.exe" /nologo /c /EHsc /MD /DNDEBUG /DUNICODE /D_UNICODE ^
  /I"%VCINC%" /I"%SDKINC%" ^
  "%SRCDIR%\RidingBootstrap.cpp" ^
  /Fo"%OUTDIR%\RidingBootstrap.obj"
if errorlevel 1 goto :fail

call "%VC%\link.exe" /nologo /DLL ^
  /OUT:"%OUTDIR%\RidingBootstrap.dll" ^
  "%OUTDIR%\RidingBootstrap.obj" ^
  /LIBPATH:"%VCLIB%" /LIBPATH:"%SDKLIB%" ^
  kernel32.lib user32.lib
if errorlevel 1 goto :fail

rem ---- Launcher EXE ----
call "%VC%\cl.exe" /nologo /c /EHsc /MD /DNDEBUG /DUNICODE /D_UNICODE ^
  /I"%VCINC%" /I"%SDKINC%" ^
  "%SRCDIR%\RidingLauncher.cpp" ^
  /Fo"%OUTDIR%\RidingLauncher.obj"
if errorlevel 1 goto :fail

call "%VC%\link.exe" /nologo /SUBSYSTEM:CONSOLE ^
  /OUT:"%OUTDIR%\RidingLauncher.exe" ^
  "%OUTDIR%\RidingLauncher.obj" ^
  /LIBPATH:"%VCLIB%" /LIBPATH:"%SDKLIB%" ^
  kernel32.lib user32.lib
if errorlevel 1 goto :fail

echo.
echo BUILD OK: %OUTDIR%\RidingBootstrap.dll
echo BUILD OK: %OUTDIR%\RidingLauncher.exe
if not exist "%OUTDIR%\tools" mkdir "%OUTDIR%\tools"
copy /Y "D:\KenshiModDev\RidingPlugin\.tmp_re_kenshi_v035_loose\tools\courgette64.exe" "%OUTDIR%\tools\courgette64.exe" >nul
copy /Y "D:\KenshiModDev\RidingPlugin\.tmp_re_kenshi_v035_loose\tools\kenshi_x64.exe.patch" "%OUTDIR%\tools\kenshi_x64.exe.patch" >nul
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
