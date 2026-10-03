@echo off
rem Builds the 32-bit xinput1_3.dll proxy and its test tools into build\
rem Needs Visual Studio 2022 (any edition) with "Desktop development with C++".
setlocal
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSINSTALLER%\vswhere.exe" (
  echo Visual Studio not found. Install Visual Studio 2022 with "Desktop development with C++".
  exit /b 1
)
rem Visual Studio's own setup scripts run vswhere.exe by name, so put it on PATH.
set "PATH=%VSINSTALLER%;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio C++ tools not found. Add "Desktop development with C++" in the Visual Studio Installer.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

cl /nologo /std:c++17 /O2 /W4 /EHsc /MT /LD src\xinput_proxy.cpp /Fobuild\ /Fe:build\xinput1_3.dll ^
   /link /DEF:src\xinput1_3.def user32.lib || exit /b 1
cl /nologo /std:c++17 /O2 /W4 /EHsc /MT tools\probe.cpp /Fobuild\ /Fe:build\probe.exe || exit /b 1
rem hooktest imports XInput through the proxy's own import library, like the game does
cl /nologo /std:c++17 /O2 /W4 /EHsc /MT tools\hooktest.cpp /Fobuild\ /Fe:build\hooktest.exe ^
   /link build\xinput1_3.lib || exit /b 1
copy /y xinput_proxy.ini build\ >nul
echo Build OK
