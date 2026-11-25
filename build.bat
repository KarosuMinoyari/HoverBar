@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio Build Tools wurden nicht gefunden.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo C++ Build Tools wurden nicht gefunden.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul

if not exist build mkdir build

rc /nologo /fo build\HoverBar.res res\HoverBar.rc
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O1 /MT /EHsc /utf-8 /W3 /DUNICODE /D_UNICODE /I third_party\WebView2\include ^
   /Fo:build\ /Fe:build\HoverBar.exe src\main.cpp src\media.cpp src\music.cpp src\games.cpp src\plugins.cpp src\chat.cpp src\update.cpp src\qrcode.cpp build\HoverBar.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\HoverBar.manifest ^
   user32.lib gdi32.lib shell32.lib ole32.lib dwmapi.lib msimg32.lib comctl32.lib advapi32.lib shcore.lib windowscodecs.lib comdlg32.lib winmm.lib bthprops.lib windowsapp.lib psapi.lib third_party\WebView2\x64\WebView2LoaderStatic.lib
if errorlevel 1 exit /b 1

echo.
echo Fertig: build\HoverBar.exe
