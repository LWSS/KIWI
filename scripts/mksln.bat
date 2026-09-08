@echo off
setlocal EnableDelayedExpansion

if "%~1"=="" (
    set "BUILD_TYPE=Debug"
) else (
    set "BUILD_TYPE=%~1"
)

if "%~2"=="" (
    set "BUILD_PLATFORM=x64"
    set "BUILD_DIR=build-x64"
) else (
    if /I "%~2"=="x64" (
        set "BUILD_PLATFORM=x64"
        set "BUILD_DIR=build-x64"
    ) else if /I "%~2"=="Win32" (
        set "BUILD_PLATFORM=Win32"
        set "BUILD_DIR=build-Win32"
    ) else (
        echo Unsupported platform: %~2. Use x64 or Win32.
        exit /b 1
    )
)

if "%BUILD_PLATFORM%"=="x64" (
    set "SOLUTION_NAME=KIWI64"
) else (
    set "SOLUTION_NAME=KIWI"
)

if exist CMakeLists.txt (
    cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A %BUILD_PLATFORM% -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
    set "CONFIGURE_RESULT=!ERRORLEVEL!"
    if !CONFIGURE_RESULT! equ 0 (
        echo Open "%CD%\%BUILD_DIR%\%SOLUTION_NAME%.sln" in Visual Studio.
        echo Select %BUILD_TYPE%^|%BUILD_PLATFORM%.
    )
    exit /b !CONFIGURE_RESULT!
) else (
    echo You must run this from the ROOT directory
    echo Usage: scripts\mksln.bat [Debug^|Release] [Win32^|x64]
    exit /b 1
)

