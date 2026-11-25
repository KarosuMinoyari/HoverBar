@echo off
rem Baut dist\setupHoverBar.exe. Voraussetzung: dist\HoverBar.exe (vom Release-Skript oder per Hand dorthin kopiert).
setlocal
cd /d "%~dp0\.."

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo C++ Build Tools wurden nicht gefunden.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul

if not exist dist\HoverBar.exe (
  echo dist\HoverBar.exe fehlt.
  exit /b 1
)
if not exist build mkdir build

rc /nologo /fo build\setup.res setup\setup.rc
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O1 /MT /EHsc /utf-8 /W3 /DUNICODE /D_UNICODE /I src ^
   /Fo:build\ /Fe:dist\setupHoverBar.exe setup\setup.cpp build\setup.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\HoverBar.manifest ^
   user32.lib shell32.lib ole32.lib comctl32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo.
echo Fertig: dist\setupHoverBar.exe
