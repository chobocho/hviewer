@echo off
REM hview 단위 테스트 빌드 + 실행 (MSVC)
REM Visual Studio Developer Command Prompt에서 실행.

cl /nologo /O2 /MT /W3 /I.. test_main.c /Fe:hview_tests.exe /link /SUBSYSTEM:CONSOLE
if exist test_main.obj del test_main.obj
if errorlevel 1 exit /b 1

echo.
echo 빌드 완료: hview_tests.exe
echo.

hview_tests.exe
