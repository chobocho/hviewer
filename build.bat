@echo off
REM MSVC build for hview - works from a plain cmd prompt.
REM Auto-detects Visual Studio via vswhere and runs VsDevCmd if needed.
REM
REM Compile flags:
REM   /MT     static CRT (single-file exe)
REM   /O2     optimize for speed
REM   /W3     warning level 3
REM   /utf-8  treat source and runtime strings as UTF-8 (preserves Korean)

setlocal
cd /d "%~dp0"

REM Skip VS setup only if a full Developer environment is already loaded.
REM (cl alone on PATH is not enough - INCLUDE/LIB must also be set.)
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
    echo         Install the "Desktop development with C++" workload.
    exit /b 1
)

if not exist "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" (
    echo [ERROR] VsDevCmd.bat not found under: %VSINSTALL%
    exit /b 1
)

REM VsDevCmd internally calls vswhere - make sure it's on PATH.
for %%i in ("%VSWHERE%") do set "PATH=%%~dpi;%PATH%"

call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo
if %errorlevel% neq 0 (
    echo [ERROR] VsDevCmd.bat failed.
    exit /b 1
)

REM VsDevCmd may change the working directory - return to script folder.
cd /d "%~dp0"

:compile
rc /nologo /fo hview.res hview.rc
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Resource compile failed.
    exit /b 1
)

cl /nologo /O2 /MT /W3 /utf-8 hview.c hview.res ^
   user32.lib gdi32.lib comdlg32.lib shell32.lib advapi32.lib ^
   /link /SUBSYSTEM:WINDOWS /OUT:hview.exe
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Build failed.
    exit /b 1
)

if exist hview.obj del hview.obj
if exist hview.res del hview.res
echo.
echo Build OK: hview.exe
endlocal

pause