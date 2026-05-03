@echo off
REM hview unit tests build + run (MSVC) - works from a plain cmd prompt.
REM Auto-detects Visual Studio via vswhere and runs VsDevCmd if needed.
REM
REM Compile flags:
REM   /MT     static CRT
REM   /O2     optimize for speed
REM   /W3     warning level 3
REM   /utf-8  treat source and runtime strings as UTF-8 (preserves Korean)

setlocal
cd /d "%~dp0"

if defined INCLUDE if defined LIB (
    where cl >nul 2>nul && goto :compile
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Install the Visual Studio Installer.
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)
if not defined VSINSTALL (
    echo [ERROR] No Visual Studio install with the MSVC C++ toolset was found.
    exit /b 1
)
if not exist "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" (
    echo [ERROR] VsDevCmd.bat not found under: %VSINSTALL%
    exit /b 1
)

for %%i in ("%VSWHERE%") do set "PATH=%%~dpi;%PATH%"

call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo
if %errorlevel% neq 0 (
    echo [ERROR] VsDevCmd.bat failed.
    exit /b 1
)
cd /d "%~dp0"

:compile
cl /nologo /O2 /MT /W3 /utf-8 /I.. test_main.c /Fe:hview_tests.exe ^
   /link /SUBSYSTEM:CONSOLE
if exist test_main.obj del test_main.obj
if errorlevel 1 (
    echo.
    echo [ERROR] Tests build failed.
    exit /b 1
)

echo.
echo Build OK: hview_tests.exe
echo.

call "%~dp0hview_tests.exe"
endlocal
