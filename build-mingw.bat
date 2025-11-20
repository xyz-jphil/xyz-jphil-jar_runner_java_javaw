@echo off
REM Build script for MinGW GCC compiler
REM This script builds jarrunner.exe using MinGW

echo Building jarrunner.exe with MinGW...
echo.

REM NOTE: Using -mconsole instead of -mwindows so parent shell waits for us
REM We'll handle GUI mode by FreeConsole() in the code
gcc -o jarrunner.exe launcher.c -mconsole -s -O2

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo BUILD SUCCESSFUL!
    echo ========================================
    echo Output: jarrunner.exe
    echo.
    dir jarrunner.exe
    echo.
) else (
    echo.
    echo ========================================
    echo BUILD FAILED!
    echo ========================================
    echo Please ensure MinGW is installed and in PATH.
    echo.
    exit /b 1
)
