@echo off
REM MSVC 빌드 — Visual Studio Developer Command Prompt에서 실행
REM /MT: CRT 정적 링크 (단일 exe)
REM /O2: 속도 최적화
REM /W3: 경고 레벨 3

cl /nologo /O2 /MT /W3 hview.c ^
   user32.lib gdi32.lib comdlg32.lib shell32.lib advapi32.lib ^
   /link /SUBSYSTEM:WINDOWS /OUT:hview.exe

if exist hview.obj del hview.obj
echo.
echo 빌드 완료: hview.exe
