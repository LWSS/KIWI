@echo off
setlocal EnableDelayedExpansion

if "%~1"=="" (
    set "BUILD_TYPE=Debug"
) else (
    set "BUILD_TYPE=%~1"
)

if "%~2"=="" (
    set "BUILD_PLATFORM=Win32"
    set "BUILD_DIR=build"
) else (
    set "BUILD_PLATFORM=%~2"
    if /I "%~2"=="x64" (
        set "BUILD_DIR=build-cod4rad"
    ) else (
        set "BUILD_DIR=build-%~2"
    )
)

if exist CMakeLists.txt (
    pushd
    if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
    cd "%BUILD_DIR%"
    cmake -G "Visual Studio 17 2022" -A %BUILD_PLATFORM% -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ..
    popd
) else (
    echo You must run this from the ROOT directory
    echo Usage: scripts\mksln.bat [Debug^|Release] [Win32^|x64]
    exit /b 1
)

