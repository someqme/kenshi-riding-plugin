@echo off
setlocal

rem ============================================
rem  RidingPlugin build script (v100 toolset)
rem ============================================

set "OUTDIR=D:\KenshiModDev\Build\RidingPlugin"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem ---- compile ----
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\cl.exe" /nologo /c /EHsc /MD /GL /DNDEBUG /D_CONSOLE /DUNICODE /D_UNICODE /DBOOST_ALL_NO_LIB ^
  /I"C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\include" ^
  /I"C:\Program Files\Microsoft SDKs\Windows\v7.1\Include" ^
  /I"D:\KenshiModDev\KenshiLib_Examples_deps\KenshiLib\Include" ^
  /I"D:\KenshiModDev\KenshiLib_Examples_deps\KenshiLib\Include\ogre" ^
  /I"D:\KenshiModDev\KenshiLib_Examples_deps\boost_1_60_0" ^
  "D:\KenshiModDev\RidingPlugin\RidingPlugin.cpp" ^
  /Fo"%OUTDIR%\RidingPlugin.obj"
if errorlevel 1 goto :fail

rem ---- link ----
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\link.exe" /nologo /DLL /LTCG ^
  /OUT:"%OUTDIR%\RidingPlugin.dll" ^
  "%OUTDIR%\RidingPlugin.obj" ^
  "D:\KenshiModDev\KenshiLib_Examples_deps\KenshiLib\Libraries\KenshiLib.lib" ^
  "D:\KenshiModDev\KenshiLib_Examples_deps\KenshiLib\Libraries\MyGUIEngine_x64.lib" ^
  "D:\KenshiModDev\KenshiLib_Examples_deps\KenshiLib\Libraries\OgreMain_x64.lib" ^
  "D:\KenshiModDev\KenshiLib_Examples_deps\boost_1_60_0\stage\lib\libboost_system-vc100-mt-1_60.lib" ^
  /LIBPATH:"C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\lib\amd64" ^
  /LIBPATH:"C:\Program Files\Microsoft SDKs\Windows\v7.1\Lib\x64" ^
  /LIBPATH:"D:\KenshiModDev\KenshiLib_Examples_deps\boost_1_60_0\stage\lib" ^
  /LIBPATH:"D:\steam\steamapps\common\Kenshi"
if errorlevel 1 goto :fail

echo.
echo BUILD OK: %OUTDIR%\RidingPlugin.dll
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
