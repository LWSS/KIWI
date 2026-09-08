@echo off
setlocal
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"
set "BUILD_PLATFORM=%~2"
if "%BUILD_PLATFORM%"=="" set "BUILD_PLATFORM=x64"

call scripts\mksln.bat "%BUILD_TYPE%" "%BUILD_PLATFORM%"
if errorlevel 1 exit /b %ERRORLEVEL%

if /I "%BUILD_PLATFORM%"=="x64" (
    set "BUILD_DIR=build-x64"
) else (
    set "BUILD_DIR=build-Win32"
)
cmake --build "%BUILD_DIR%" --config "%BUILD_TYPE%" --target KIWI-mp KIWI-dedi
exit /b %ERRORLEVEL%
